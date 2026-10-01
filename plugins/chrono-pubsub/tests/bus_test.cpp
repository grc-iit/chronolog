#include "chronolog/pubsub/bus.h"
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace chronolog;
using namespace std::chrono_literals;
namespace
{
std::string catalog, player, scratch;
client::Client connect()
{
    client::ClientOptions options;
    options.catalog_endpoint = catalog;
    options.player_endpoint = player;
    options.rpc_timeout = 1s;
    auto c = client::Client::Connect(options);
    if(!c.ok())
        throw std::runtime_error(c.status().ToString());
    return std::move(*c);
}
TEST(Pubsub, EarliestRetriesCallbackWithIdentityAndMetadata)
{
    auto c = connect();
    auto other = connect();
    pubsub::Bus producer(c, "pubsub-earliest"), consumer(other, "pubsub-earliest");
    pubsub::PublishOptions options;
    options.metadata.content_type = "application/json";
    options.metadata.trace_id = std::string(16, 't');
    options.metadata.span_id = std::string(8, 's');
    options.metadata.attributes["gen_ai.agent.id"] = "worker";
    auto first = producer.publish("memory", "one", options);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first->acked());
    auto second = producer.publish("memory", "two", options);
    ASSERT_TRUE(second.ok());
    auto third = producer.publish("memory", "three", options);
    ASSERT_TRUE(third.ok());
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;
    std::atomic<int> first_attempts{};
    auto main_thread = std::this_thread::get_id();
    std::thread::id callback_thread;
    auto subscription =
            consumer.subscribe("memory",
                               [&](const pubsub::Message& message)
                               {
                                   if(message.event.id == first->event_id && first_attempts.fetch_add(1) == 0)
                                       return absl::UnavailableError("retry processing");
                                   std::lock_guard lock(mutex);
                                   if(events.size() >= 3)
                                       return absl::ResourceExhaustedError("unexpected extra delivery");
                                   callback_thread = std::this_thread::get_id();
                                   events.push_back(message.event);
                                   changed.notify_all();
                                   return absl::OkStatus();
                               },
                               {.start = pubsub::Start::Earliest, .deadline = std::chrono::system_clock::now() + 8s});
    ASSERT_TRUE(subscription.ok()) << subscription.status();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return events.size() == 3; }));
    }
    (*subscription)->stop();
    EXPECT_NE(main_thread, callback_thread);
    EXPECT_EQ(first_attempts.load(), 2);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].id, first->event_id);
    EXPECT_EQ(events[1].id, second->event_id);
    EXPECT_EQ(events[2].id, third->event_id);
    EXPECT_EQ(events[0].envelope.trace_id, options.metadata.trace_id);
    EXPECT_EQ(events[0].envelope.span_id, options.metadata.span_id);
    EXPECT_EQ(events[0].envelope.content_type, "application/json");
    EXPECT_EQ(events[0].envelope.attributes.at("gen_ai.agent.id"), "worker");
    EXPECT_TRUE((*subscription)->status().ok());
    ASSERT_TRUE((*subscription)->position());
    EXPECT_EQ((*subscription)->position()->id, third->event_id);
    EXPECT_FALSE((*subscription)->completion().has_value());
}
TEST(Pubsub, LatestAndDurableSavedKvsPositionResume)
{
    auto c = connect();
    auto other = connect();
    pubsub::Bus producer(c, "pubsub-saved"), consumer(other, "pubsub-saved");
    auto old = producer.publish("memory", "old");
    ASSERT_TRUE(old.ok());
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<EventId> delivered;
    auto callback = [&](const pubsub::Message& message)
    {
        std::lock_guard lock(mutex);
        if(delivered.size() >= 2)
            return absl::ResourceExhaustedError("extra delivery");
        delivered.push_back(message.event.id);
        changed.notify_all();
        return absl::OkStatus();
    };
    auto latest = consumer.subscribe("memory", callback);
    ASSERT_TRUE(latest.ok()) << latest.status();
    auto one = producer.publish("memory", "one");
    ASSERT_TRUE(one.ok());
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return delivered.size() == 1; }));
    }
    (*latest)->stop();
    ASSERT_TRUE((*latest)->position());
    EXPECT_EQ(delivered[0], one->event_id);
    kvs::Store checkpoint(c, "pubsub-checkpoints");
    auto saved = pubsub::savePosition(checkpoint, "consumer", *(*latest)->position());
    ASSERT_TRUE(saved.ok());
    EXPECT_TRUE(saved->acked());
    auto next = producer.publish("memory", "next");
    ASSERT_TRUE(next.ok());
    auto fresh = connect();
    kvs::Store restored(fresh, "pubsub-checkpoints");
    auto position = pubsub::loadPosition(restored, "consumer", {.causal_floor = saved->hlc});
    ASSERT_TRUE(position.ok()) << position.status();
    EXPECT_EQ(position->id, one->event_id);
    pubsub::Bus resumed(fresh, "pubsub-saved");
    auto following = resumed.subscribe("memory", callback, {.start = pubsub::Start::Saved, .position = *position});
    ASSERT_TRUE(following.ok()) << following.status();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return delivered.size() == 2; }));
    }
    (*following)->stop();
    EXPECT_EQ(delivered[1], next->event_id);
}
TEST(Pubsub, LatestFrontierCheckpointResumesBeforeAnyDelivery)
{
    auto c = connect();
    pubsub::Bus producer(c, "pubsub-frontier");
    auto old = producer.publish("memory", "old");
    ASSERT_TRUE(old.ok());
    auto latest = producer.subscribe("memory", [](const pubsub::Message&) { return absl::OkStatus(); });
    ASSERT_TRUE(latest.ok());
    ASSERT_TRUE((*latest)->position());
    auto frontier = *(*latest)->position();
    (*latest)->stop();
    kvs::Store checkpoints(c, "pubsub-frontier-checkpoints");
    auto saved = pubsub::savePosition(checkpoints, "consumer", frontier);
    ASSERT_TRUE(saved.ok()) << saved.status();
    auto next = producer.publish("memory", "new");
    ASSERT_TRUE(next.ok());
    auto other = connect();
    kvs::Store restored(other, "pubsub-frontier-checkpoints");
    auto position = pubsub::loadPosition(restored, "consumer", {.causal_floor = saved->hlc});
    ASSERT_TRUE(position.ok()) << position.status();
    pubsub::Bus consumer(other, "pubsub-frontier");
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<EventId> received;
    auto resumed = consumer.subscribe("memory",
                                      [&](const pubsub::Message& message)
                                      {
                                          std::lock_guard lock(mutex);
                                          received = message.event.id;
                                          changed.notify_all();
                                          return absl::OkStatus();
                                      },
                                      {.start = pubsub::Start::Saved, .position = *position});
    ASSERT_TRUE(resumed.ok());
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return received.has_value(); }));
    }
    (*resumed)->stop();
    EXPECT_EQ(*received, next->event_id);
}
TEST(Pubsub, ValidationDeadlineAndCallbackStop)
{
    auto c = connect();
    pubsub::Bus bus(c, "pubsub-validation");
    EXPECT_TRUE(absl::IsInvalidArgument(bus.publish("bad\nname", "x").status()));
    EXPECT_TRUE(absl::IsInvalidArgument(bus.subscribe("topic", {}).status()));
    auto callback = [](const pubsub::Message&) { return absl::OkStatus(); };
    EXPECT_TRUE(absl::IsInvalidArgument(bus.subscribe("topic", callback, {.start = pubsub::Start::Saved}).status()));
    auto first = bus.publish("topic", "one");
    ASSERT_TRUE(first.ok());
    client::Position wrong{first->hlc, first->event_id};
    EXPECT_TRUE(absl::IsInvalidArgument(
            bus.subscribe("another", callback, {.start = pubsub::Start::Saved, .position = wrong}).status()));
    auto short_lived = bus.subscribe(
            "idle",
            callback,
            {.deadline = std::chrono::system_clock::now() + 100ms, .pull_timeout = 20ms, .retry_delay = 10ms});
    ASSERT_TRUE(short_lived.ok());
    for(int attempt = 0; attempt < 100 && (*short_lived)->status().ok(); ++attempt) std::this_thread::sleep_for(10ms);
    (*short_lived)->stop();
    EXPECT_TRUE(absl::IsDeadlineExceeded((*short_lived)->status()));
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    std::atomic<pubsub::Subscription*> handle{};
    auto self = bus.subscribe("self",
                              [&](const pubsub::Message&)
                              {
                                  handle.load()->requestStop();
                                  std::lock_guard lock(mutex);
                                  stopped = true;
                                  changed.notify_all();
                                  return absl::OkStatus();
                              });
    ASSERT_TRUE(self.ok());
    handle.store(self->get());
    ASSERT_TRUE(bus.publish("self", "stop").ok());
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return stopped; }));
    }
    (*self)->stop();
    EXPECT_TRUE((*self)->status().ok());
    kvs::Store positions(c, "pubsub-invalid-positions");
    EXPECT_TRUE(absl::IsInvalidArgument(pubsub::savePosition(positions, "invalid", {}).status()));
    ASSERT_TRUE(positions.put("invalid", "not a position").ok());
    EXPECT_TRUE(absl::IsInvalidArgument(pubsub::loadPosition(positions, "invalid").status()));
    kvs::PutOptions metadata;
    metadata.metadata.content_type = "application/vnd.chronolog.pubsub.position";
    ASSERT_TRUE(positions.put("overflow", "0 4294967296 1 1 1 1", metadata).ok());
    EXPECT_TRUE(absl::IsInvalidArgument(pubsub::loadPosition(positions, "overflow").status()));
    EXPECT_TRUE(absl::IsNotFound(pubsub::loadPosition(positions, "absent").status()));
}
TEST(Pubsub, PlayerRestartKeepsEveryEventAndCursor)
{
    auto c = connect();
    auto other = connect();
    pubsub::Bus producer(c, "pubsub-restart"), consumer(other, "pubsub-restart");
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<EventId> delivered, published;
    auto subscription =
            consumer.subscribe("memory",
                               [&](const pubsub::Message& message)
                               {
                                   std::lock_guard lock(mutex);
                                   if(delivered.size() >= 40)
                                       return absl::ResourceExhaustedError("duplicate delivery after restart");
                                   delivered.push_back(message.event.id);
                                   changed.notify_all();
                                   return absl::OkStatus();
                               },
                               {.start = pubsub::Start::Earliest, .deadline = std::chrono::system_clock::now() + 20s});
    ASSERT_TRUE(subscription.ok());
    for(int i = 0; i < 20; ++i)
    {
        auto p = producer.publish("memory", std::to_string(i));
        ASSERT_TRUE(p.ok());
        published.push_back(p->event_id);
    }
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return delivered.size() == 20; }));
    }
    std::ofstream(scratch + "/restart-request").put('1');
    bool down = false;
    for(int i = 0; i < 100 && !down; ++i)
    {
        down = std::filesystem::exists(scratch + "/player-stopped");
        if(!down)
            std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(down);
    for(int i = 20; i < 40; ++i)
    {
        auto p = producer.publish("memory", std::to_string(i));
        ASSERT_TRUE(p.ok());
        published.push_back(p->event_id);
    }
    std::ofstream(scratch + "/resume-request").put('1');
    bool restarted = false;
    for(int i = 0; i < 100 && !restarted; ++i)
    {
        restarted = std::filesystem::exists(scratch + "/player-restarted");
        if(!restarted)
            std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(restarted);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 8s, [&] { return delivered.size() == 40; }));
    }
    (*subscription)->stop();
    EXPECT_EQ(delivered, published);
    EXPECT_TRUE((*subscription)->status().ok());
    ASSERT_TRUE((*subscription)->position());
    EXPECT_EQ((*subscription)->position()->id, published.back());
    EXPECT_TRUE(std::filesystem::exists(scratch + "/player-restarted"));
}
} // namespace
int main(int argc, char** argv)
{
    if(argc != 4)
        return 2;
    catalog = argv[1];
    player = argv[2];
    scratch = argv[3];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
