#include "chronolog/client/client.h"
#include <iostream>
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
    options.player_endpoint = argv[2];
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
    REQUIRE(client->destroyStory(story->id).ok());
    REQUIRE(client->destroyChronicle("sdk-acceptance").ok());
    std::cout << "SDK ACCEPTANCE PASSED\n";
}
