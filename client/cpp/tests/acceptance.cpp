#include "chronolog/client/client.h"
#include <iostream>
#include <set>
#include <thread>

namespace sdk = chronolog::client;
using namespace chronolog;
using namespace std::chrono_literals;
#define REQUIRE(expr)                                                                                                  \
    do {                                                                                                               \
        if(!(expr))                                                                                                    \
        {                                                                                                              \
            std::cerr << "FAIL " #expr << '\n';                                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while(false)
int main(int argc, char** argv)
{
    REQUIRE(argc == 3);
    sdk::ClientOptions options;
    options.catalog_endpoint = argv[1];
    (void)argv[2];
    options.rpc_timeout = 10s;
    auto client = sdk::Client::Connect(options);
    REQUIRE(client.ok());
    REQUIRE(client->createChronicle("sdk-acceptance").ok());
    auto chronicle = client->getChronicle("sdk-acceptance");
    REQUIRE(chronicle.ok());
    auto chronicles = client->listChronicles();
    REQUIRE(chronicles.ok() && chronicles->size() == 1);
    auto story = client->createStory("sdk-acceptance", "events");
    REQUIRE(story.ok());
    REQUIRE(client->getStory(story->id).ok());
    auto stories = client->listStories("sdk-acceptance");
    REQUIRE(stories.ok() && stories->size() == 1);
    auto writer = client->acquire(story->id, "writer");
    REQUIRE(writer.ok());
    auto acquired = writer->acquisition();
    REQUIRE(acquired.incarnation == 1);
    std::vector<sdk::AppendSpec> specs(1000);
    for(size_t i = 0; i < specs.size(); ++i)
    {
        specs[i].envelope.payload = "event-" + std::to_string(i + 1);
        specs[i].envelope.attributes["test"] = "sdk";
    }
    auto appended = writer->appendBatch(specs);
    REQUIRE(appended.ok());
    REQUIRE(appended->size() == 1000);
    for(size_t i = 0; i < appended->size(); ++i)
    {
        REQUIRE((*appended)[i].ok());
        REQUIRE((*appended)[i]->acked());
        REQUIRE((*appended)[i]->event_id.sequence == i + 1);
    }
    auto start = (*appended)[0]->hlc;
    auto end = (*appended)[999]->hlc;
    ++end.logical;
    std::vector<Event> events;
    bool complete = false;
    for(int attempt = 0; attempt < 30 && !complete; ++attempt)
    {
        auto read = client->read(story->id, {start, end});
        REQUIRE(read.ok());
        events.clear();
        for(size_t pull = 0; pull < 1020; ++pull)
        {
            auto item = read->next();
            if(!item.ok() && (item.status().code() == absl::StatusCode::kFailedPrecondition ||
                              item.status().code() == absl::StatusCode::kUnavailable))
                break;
            if(!item.ok())
                std::cerr << "Read pull: " << item.status() << '\n';
            REQUIRE(item.ok());
            if(!*item)
                break;
            if((**item).completion)
                complete = (**item).completion->complete;
            for(auto& event: (**item).events) events.push_back(std::move(event));
        }
        if(!complete)
            std::this_thread::sleep_for(100ms);
    }
    REQUIRE(complete && events.size() == 1000);
    for(size_t i = 0; i < events.size(); ++i)
    {
        REQUIRE(events[i].id == (*appended)[i]->event_id);
        REQUIRE(events[i].id.story_id == story->id);
        REQUIRE(events[i].hlc == (*appended)[i]->hlc);
        REQUIRE(events[i].envelope.payload == specs[i].envelope.payload);
        REQUIRE(events[i].envelope.attributes == specs[i].envelope.attributes);
    }
    auto tail = client->tail(story->id, sdk::Position{events[499].hlc, events[499].id});
    REQUIRE(tail.ok());
    size_t delivered = 0;
    for(size_t pull = 0; pull < 510 && delivered < 500; ++pull)
    {
        auto item = tail->next();
        REQUIRE(item.ok() && *item);
        for(const auto& event: (**item).events)
        {
            REQUIRE(delivered < 500);
            REQUIRE(event.id == events[500 + delivered].id);
            ++delivered;
        }
    }
    REQUIRE(delivered == 500);
    tail->cancel();
    sdk::AppendSpec rejectionSpec;
    rejectionSpec.envelope.payload = "backdated";
    rejectionSpec.physical = TimeReading{1, {}, ClockStatus::Unsynced};
    auto rejection = writer->append(rejectionSpec);
    REQUIRE(rejection.status().code() == absl::StatusCode::kOutOfRange);
    sdk::AppendSpec afterRejection;
    afterRejection.envelope.payload = "after-rejection";
    auto resumed = writer->append(afterRejection);
    REQUIRE(resumed.ok() && resumed->event_id.sequence == 1002);

    constexpr int threads = 4, perThread = 25;
    std::vector<std::thread> pool;
    std::vector<int> failed(threads, 0);
    std::vector<uint64_t> writerIds(threads, 0);
    for(int t = 0; t < threads; ++t)
        pool.emplace_back(
                [&, t]
                {
                    auto concurrent = client->acquire(story->id, "thread-" + std::to_string(t));
                    if(!concurrent.ok())
                    {
                        failed[t] = 1;
                        return;
                    }
                    writerIds[t] = concurrent->acquisition().writer_id;
                    for(int i = 1; i <= perThread; ++i)
                    {
                        sdk::AppendSpec spec;
                        spec.envelope.payload = "t" + std::to_string(t) + "-" + std::to_string(i);
                        auto result = concurrent->append(spec);
                        if(!result.ok() || !result->acked() || result->event_id.sequence != static_cast<uint64_t>(i) ||
                           result->event_id.writer_id != writerIds[t])
                        {
                            failed[t] = 1;
                            break;
                        }
                    }
                    if(!concurrent->release().ok())
                        failed[t] = 1;
                });
    for(auto& worker: pool) worker.join();
    for(int flag: failed) REQUIRE(flag == 0);
    REQUIRE(std::set<uint64_t>(writerIds.begin(), writerIds.end()).size() == static_cast<size_t>(threads));

    auto released = writer->release();
    REQUIRE(released.ok() && *released);
    auto rejected = writer->append({{"", "released", "", "", {}}});
    REQUIRE(!rejected.ok() && rejected.status().code() == absl::StatusCode::kFailedPrecondition);
    auto again = client->acquire(story->id, "writer");
    REQUIRE(again.ok());
    REQUIRE(again->acquisition().incarnation == 2 && again->acquisition().writer_id == acquired.writer_id);
    auto first = again->append({{"", "new incarnation", "", "", {}}});
    REQUIRE(first.ok() && first->event_id.sequence == 1 && first->acked());
    REQUIRE(again->release().ok());
    auto largeStory = client->createStory("sdk-acceptance", "receive-limit");
    REQUIRE(largeStory.ok());
    auto largeWriter = client->acquire(largeStory->id, "receive-limit-writer");
    REQUIRE(largeWriter.ok());
    std::vector<sdk::AppendSpec> largeSpecs(8);
    for(size_t i = 0; i < largeSpecs.size(); ++i)
        largeSpecs[i].envelope.payload = std::string(1024 * 1024, static_cast<char>(i));
    auto largeResults = largeWriter->appendBatch(largeSpecs);
    REQUIRE(largeResults.ok() && largeResults->size() == 8);
    for(const auto& result: *largeResults) REQUIRE(result.ok() && result->acked());
    auto largeEnd = largeResults->back()->hlc;
    ++largeEnd.logical;
    complete = false;
    for(int attempt = 0; attempt < 30 && !complete; ++attempt)
    {
        auto read = client->read(largeStory->id, {largeResults->front()->hlc, largeEnd});
        REQUIRE(read.ok());
        events.clear();
        for(size_t pull = 0; pull < 10; ++pull)
        {
            auto item = read->next();
            REQUIRE(item.ok());
            if(!*item)
                break;
            if((**item).completion)
                complete = (**item).completion->complete;
            for(auto& event: (**item).events) events.push_back(std::move(event));
        }
        if(!complete)
            std::this_thread::sleep_for(100ms);
    }
    REQUIRE(complete && events.size() == 8);
    for(size_t i = 0; i < events.size(); ++i)
    {
        REQUIRE(events[i].id == (*largeResults)[i]->event_id);
        REQUIRE(events[i].envelope.payload == largeSpecs[i].envelope.payload);
    }
    auto largeTail = client->tail(largeStory->id, {}, std::chrono::system_clock::now() + 10s);
    REQUIRE(largeTail.ok());
    delivered = 0;
    for(size_t pull = 0; pull < 10 && delivered < 8; ++pull)
    {
        auto item = largeTail->next();
        REQUIRE(item.ok() && *item);
        for(const auto& event: (**item).events)
        {
            REQUIRE(delivered < 8);
            REQUIRE(event.id == (*largeResults)[delivered]->event_id);
            REQUIRE(event.envelope.payload == largeSpecs[delivered].envelope.payload);
            ++delivered;
        }
    }
    REQUIRE(delivered == 8);
    largeTail->cancel();
    REQUIRE(largeWriter->release().ok());
    REQUIRE(client->destroyStory(largeStory->id).ok());
    REQUIRE(client->destroyStory(story->id).ok());
    REQUIRE(client->destroyChronicle("sdk-acceptance").ok());
    std::cout << "SDK ACCEPTANCE PASSED\n";
}
