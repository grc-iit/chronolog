#include "chronolog/pubsub/bus.h"
#include <condition_variable>
#include <iostream>

int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    chronolog::client::ClientOptions options;
    options.catalog_endpoint = argv[1];
    options.player_endpoint = argv[2];
    auto publisher = chronolog::client::Client::Connect(options);
    auto consumer = chronolog::client::Client::Connect(options);
    if(!publisher.ok() || !consumer.ok())
        return 1;
    chronolog::pubsub::Bus bus(*publisher, "pubsub-example");
    chronolog::pubsub::Bus listener(*consumer, "pubsub-example");
    chronolog::kvs::Store checkpoints(*consumer, "pubsub-positions");
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<chronolog::Event> received;
    std::optional<chronolog::kvs::Version> checkpoint;
    auto subscription = listener.subscribe(
            "agent-memory",
            [&](const chronolog::pubsub::Message& message)
            {
                auto saved = chronolog::pubsub::savePosition(checkpoints, "workflow-consumer", message.position());
                if(!saved.ok())
                    return saved.status();
                if(!saved->acked())
                    return absl::UnavailableError("checkpoint not durably acknowledged");
                std::lock_guard lock(mutex);
                received = message.event;
                checkpoint = *saved;
                changed.notify_all();
                return absl::OkStatus();
            });
    if(!subscription.ok())
    {
        std::cerr << subscription.status() << '\n';
        return 1;
    }
    chronolog::pubsub::PublishOptions metadata;
    metadata.metadata.content_type = "application/json";
    metadata.metadata.attributes["gen_ai.agent.id"] = "planner";
    auto first = bus.publish("agent-memory", R"({"plan":"inspect provenance"})", metadata);
    if(!first.ok() || !first->acked())
        return 1;
    {
        std::unique_lock lock(mutex);
        if(!changed.wait_for(lock, std::chrono::seconds(5), [&] { return received.has_value(); }))
            return 1;
        if(received->id != first->event_id || received->envelope.attributes.at("gen_ai.agent.id") != "planner")
            return 1;
    }
    (*subscription)->stop();
    auto next = bus.publish("agent-memory", R"({"plan":"execute workflow"})", metadata);
    if(!next.ok())
        return 1;
    auto restarted = chronolog::client::Client::Connect(options);
    if(!restarted.ok())
        return 1;
    chronolog::kvs::Store restored(*restarted, "pubsub-positions");
    auto position = chronolog::pubsub::loadPosition(restored, "workflow-consumer", {.causal_floor = checkpoint->hlc});
    if(!position.ok())
    {
        std::cerr << position.status() << '\n';
        return 1;
    }
    chronolog::pubsub::Bus resumed(*restarted, "pubsub-example");
    {
        std::lock_guard lock(mutex);
        received.reset();
    }
    auto following = resumed.subscribe("agent-memory",
                                       [&](const chronolog::pubsub::Message& message)
                                       {
                                           std::lock_guard lock(mutex);
                                           received = message.event;
                                           changed.notify_all();
                                           return absl::OkStatus();
                                       },
                                       {.start = chronolog::pubsub::Start::Saved, .position = *position});
    if(!following.ok())
        return 1;
    {
        std::unique_lock lock(mutex);
        if(!changed.wait_for(lock, std::chrono::seconds(5), [&] { return received.has_value(); }))
            return 1;
        if(received->id != next->event_id)
            return 1;
        std::cout << "agent-memory resumed after " << position->id.sequence << ": " << received->envelope.payload
                  << '\n';
    }
    (*following)->stop();
    return 0;
}
