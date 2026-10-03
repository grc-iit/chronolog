#include "chronolog/client/client.h"
#include <fstream>
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

struct Row
{
    EventId id;
    Hlc hlc;
    size_t bytes{};
};

int main(int argc, char** argv)
{
    REQUIRE(argc == 5);
    std::string mode = argv[1];
    sdk::ClientOptions options;
    options.catalog_endpoint = argv[2];
    options.player_endpoint = argv[3];
    options.rpc_timeout = 5s;
    auto client = sdk::Client::Connect(options);
    REQUIRE(client.ok());
    if(mode == "probe")
    {
        REQUIRE(client->listChronicles(std::chrono::system_clock::now() + 1s).ok());
        auto read = client->read(UINT64_MAX, {{0, 0}, {INT64_MAX, UINT32_MAX}}, std::chrono::system_clock::now() + 1s);
        absl::Status status;
        if(!read.ok())
            status = read.status();
        else
        {
            auto item = read->next(std::chrono::system_clock::now() + 1s);
            status = item.status();
        }
        REQUIRE(status.code() == absl::StatusCode::kNotFound || status.code() == absl::StatusCode::kFailedPrecondition);
        return 0;
    }
    if(mode == "durable" || mode == "mixed" || mode == "wal100")
    {
        REQUIRE(client->createChronicle("local-test").ok());
        auto story = client->createStory("local-test", "events");
        REQUIRE(story.ok());
        auto writer = client->acquire(story->id, "local-writer");
        REQUIRE(writer.ok());
        std::ofstream out(argv[4]);
        REQUIRE(out.good());
        size_t count = mode == "wal100" ? 200 : 24;
        for(size_t i = 0; i < count; ++i)
        {
            sdk::AppendSpec spec;
            spec.envelope.payload = std::string(mode == "wal100" ? 524288 : 100, static_cast<char>('a' + i % 26));
            spec.durability = mode == "mixed" && i % 2 == 0 ? Durability::Accepted : Durability::Durable;
            auto result = writer->append(spec);
            REQUIRE(result.ok());
            REQUIRE(result->acked() == (spec.durability == Durability::Durable));
            REQUIRE(result->achieved == spec.durability);
            auto id = result->event_id;
            out << id.story_id << ' ' << id.writer_id << ' ' << id.incarnation << ' ' << id.sequence << ' '
                << result->hlc.physical_ns << ' ' << result->hlc.logical << ' ' << spec.envelope.payload.size() << '\n';
        }
        return 0;
    }
    REQUIRE(mode == "read");
    std::ifstream in(argv[4]);
    std::vector<Row> rows;
    Row row;
    while(in >> row.id.story_id >> row.id.writer_id >> row.id.incarnation >> row.id.sequence >> row.hlc.physical_ns >>
          row.hlc.logical >> row.bytes)
        rows.push_back(row);
    REQUIRE(!rows.empty());
    auto end = rows.back().hlc;
    ++end.logical;
    bool complete = false;
    std::vector<Event> events;
    for(int attempt = 0; attempt < 30 && !complete; ++attempt)
    {
        auto stream = client->read(rows.front().id.story_id, {rows.front().hlc, end});
        REQUIRE(stream.ok());
        events.clear();
        for(size_t pull = 0; pull < rows.size() + 10; ++pull)
        {
            auto item = stream->next();
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
    REQUIRE(complete && events.size() == rows.size());
    for(size_t i = 0; i < rows.size(); ++i)
    {
        REQUIRE(events[i].id == rows[i].id);
        REQUIRE(events[i].hlc == rows[i].hlc);
        REQUIRE(events[i].envelope.payload == std::string(rows[i].bytes, static_cast<char>('a' + i % 26)));
    }
    std::cout << "complete " << events.size() << '\n';
}
