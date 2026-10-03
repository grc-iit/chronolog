#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <grpcpp/grpcpp.h>
#include "chronolog/acquire_refusal.h"
#include "chronolog/client/client.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace sdk = chronolog::client;
namespace iv1 = chronolog::internal::v1;
using namespace chronolog;
using namespace std::chrono_literals;

namespace
{
struct Row
{
    EventId id;
    Hlc hlc;
    int requested{}, achieved{};
    std::string payload;
};

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "FAIL " << message << '\n';
    std::exit(1);
}

std::string path(const char* out, const char* name) { return std::string(out) + "/" + name; }

std::vector<Row> loadRows(const std::string& file)
{
    std::ifstream in(file);
    std::vector<Row> rows;
    Row row;
    while(in >> row.id.story_id >> row.id.writer_id >> row.id.incarnation >> row.id.sequence >> row.hlc.physical_ns >>
          row.hlc.logical >> row.requested >> row.achieved >> row.payload)
        rows.push_back(row);
    return rows;
}

Hlc loadHlc(const std::string& file)
{
    std::ifstream in(file);
    Hlc hlc;
    if(!(in >> hlc.physical_ns >> hlc.logical))
        fail("cannot read " + file);
    return hlc;
}

void saveHlc(const std::string& file, Hlc hlc) { std::ofstream(file) << hlc.physical_ns << ' ' << hlc.logical << '\n'; }

StoryId loadStory(const char* out)
{
    std::ifstream in(path(out, "story"));
    StoryId story = 0;
    if(!(in >> story))
        fail("cannot read story id");
    return story;
}

Hlc fromWire(const v1::Hlc& hlc) { return {hlc.physical_ns(), hlc.logical()}; }

struct Hot
{
    bool ok{};
    std::vector<Event> events;
    Hlc sealed, evicted, frontier;
};

// One FetchHot over the whole HLC axis, bounded so a misbehaving Keeper cannot stream without end.
Hot fetchHot(const std::string& endpoint, StoryId story, uint64_t max_events)
{
    Hot hot;
    auto stub = iv1::Archive::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    iv1::FetchHotRequest request;
    request.set_story_id(story);
    request.mutable_hlc()->mutable_start();
    request.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
    request.set_max_events(max_events);
    auto reader = stub->FetchHot(&context, request);
    iv1::FetchHotResponse response;
    bool trailer = false;
    for(size_t frames = 0; frames < 4096 && reader->Read(&response); ++frames)
    {
        for(const auto& wire: response.batch().events())
        {
            Event event;
            event.id = {wire.id().story_id(), wire.id().writer_id(), wire.id().incarnation(), wire.id().sequence()};
            event.hlc = fromWire(wire.hlc());
            event.envelope.payload = wire.envelope().payload();
            hot.events.push_back(std::move(event));
        }
        if(response.has_trailer())
        {
            trailer = true;
            hot.sealed = fromWire(response.trailer().sealed_frontier());
            hot.evicted = fromWire(response.trailer().evicted_below());
            hot.frontier = hot.sealed;
            for(const auto& writer: response.trailer().frontiers())
                hot.frontier = std::max(hot.frontier, fromWire(writer.frontier()));
        }
    }
    hot.ok = reader->Finish().ok() && trailer;
    return hot;
}

absl::StatusOr<sdk::Client> connect(const char* catalog)
{
    sdk::ClientOptions options;
    options.catalog_endpoint = catalog;
    options.rpc_timeout = 10s;
    return sdk::Client::Connect(options);
}

int write(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok() || !client->createChronicle("wal-restart").ok())
        fail("catalog");
    auto story = client->createStory("wal-restart", "events");
    if(!story.ok())
        fail("createStory");
    std::ofstream(path(out, "story")) << story->id << '\n';
    auto writer = client->acquire(story->id, "writer");
    if(!writer.ok())
        fail("acquire");
    // The prior incarnation is recorded beside the story; request ids never leave this process.
    std::ofstream(path(out, "incarnation")) << writer->acquisition().incarnation << '\n';
    std::ofstream rows(path(out, "events.tsv"));
    for(uint64_t i = 1; i <= 24; ++i)
    {
        sdk::AppendSpec spec;
        spec.durability = (i % 3 == 0 || i == 24) ? Durability::Accepted : Durability::Durable;
        spec.envelope.payload = "event-" + std::to_string(i);
        auto result = writer->append(spec);
        if(!result.ok())
            fail("append " + std::to_string(i) + ": " + std::string(result.status().message()));
        const auto id = result->event_id;
        rows << id.story_id << ' ' << id.writer_id << ' ' << id.incarnation << ' ' << id.sequence << ' '
             << result->hlc.physical_ns << ' ' << result->hlc.logical << ' ' << static_cast<int>(spec.durability) << ' '
             << static_cast<int>(result->achieved) << ' ' << spec.envelope.payload << '\n';
    }
    return 0;
}

int frontier(const char* keeper, const char* out)
{
    auto hot = fetchHot(keeper, loadStory(out), 1);
    if(!hot.ok)
        fail("FetchHot before the kill");
    saveHlc(path(out, "frontier"), hot.frontier);
    std::cout << "frontier " << hot.frontier.physical_ns << ' ' << hot.frontier.logical << '\n';
    return 0;
}

int append(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    const auto story = loadStory(out);
    uint64_t prior = 0;
    std::ifstream(path(out, "incarnation")) >> prior;
    if(!prior)
        fail("no recorded prior incarnation");
    // A new process recovers its own recorded prior by compare-and-swap with a fresh id. A mismatched prior
    // refuses PRIOR_MISMATCH without taking over the current holder.
    auto cas = [&](uint64_t expected)
    {
        AcquireOptions options;
        options.takeover = true;
        options.expected_prior_incarnation = expected;
        auto id = client->newAcquireRequestId();
        if(!id.ok())
            fail("newAcquireRequestId");
        options.acquire_request_id = *id;
        return client->acquire(story, "writer", options);
    };
    auto mismatch = cas(prior + 1000);
    auto refusal = getAcquireRefusal(mismatch.status());
    if(mismatch.ok() || !refusal || refusal->refusal_reason != AcquireRefusalReason::PriorMismatch ||
       refusal->current_incarnation != prior)
        fail("mismatched prior did not refuse PRIOR_MISMATCH: " + std::string(mismatch.status().message()));
    auto writer = cas(prior);
    if(!writer.ok())
        fail("reacquire: " + std::string(writer.status().message()));
    if(writer->acquisition().incarnation != prior + 1)
        fail("reacquire did not create the next incarnation");
    sdk::AppendSpec spec;
    spec.envelope.payload = "after-restart";
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while(true)
    {
        auto result = writer->append(spec);
        if(result.ok() && result->acked())
        {
            saveHlc(path(out, "post"), result->hlc);
            std::cout << "post " << result->hlc.physical_ns << ' ' << result->hlc.logical << " incarnation "
                      << result->event_id.incarnation << '\n';
            return 0;
        }
        if(std::chrono::steady_clock::now() > deadline)
            fail("append after restart: " +
                 (result.ok() ? std::string("not durable") : std::string(result.status().message())));
        std::this_thread::sleep_for(200ms);
    }
}

std::vector<Row> durable(const char* out)
{
    std::vector<Row> rows;
    for(const auto& row: loadRows(path(out, "events.tsv")))
        if(row.achieved == static_cast<int>(Durability::Durable))
            rows.push_back(row);
    return rows;
}

bool contains(const std::vector<Row>& rows, const std::vector<Event>& events, std::string& why)
{
    std::map<EventId, const Event*> seen;
    for(const auto& event: events)
        if(!seen.emplace(event.id, &event).second)
        {
            why = "duplicate event " + std::to_string(event.id.sequence);
            return false;
        }
    for(const auto& row: rows)
    {
        auto it = seen.find(row.id);
        if(it == seen.end())
        {
            why = "durable event " + std::to_string(row.id.sequence) + " is missing";
            return false;
        }
        if(it->second->hlc != row.hlc || it->second->envelope.payload != row.payload)
        {
            why = "durable event " + std::to_string(row.id.sequence) + " changed hlc or payload";
            return false;
        }
    }
    return true;
}

int hot(const char* keeper, const char* out)
{
    const auto rows = durable(out);
    if(rows.empty())
        fail("no durable events were acknowledged");
    const auto story = loadStory(out);
    std::string why = "Keeper never answered";
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto snapshot = fetchHot(keeper, story, 1000);
        if(snapshot.ok && contains(rows, snapshot.events, why))
        {
            std::cout << "hot durable=" << rows.size() << " served=" << snapshot.events.size() << '\n';
            return 0;
        }
        std::this_thread::sleep_for(200ms);
    }
    fail("replay from the WAL: " + why);
}

int verify(const char* catalog, const char* keeper, const char* out, const char* dump = nullptr)
{
    const auto rows = durable(out);
    const auto story = loadStory(out);
    Hlc last{};
    for(const auto& row: loadRows(path(out, "events.tsv"))) last = std::max(last, row.hlc);
    Hlc last_durable{};
    for(const auto& row: rows) last_durable = std::max(last_durable, row.hlc);
    const auto deadline = std::chrono::steady_clock::now() + 90s;
    Hlc evicted{};
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto snapshot = fetchHot(keeper, story, 1);
        if(snapshot.ok)
            evicted = snapshot.evicted;
        if(snapshot.ok && evicted > last_durable)
            break;
        std::this_thread::sleep_for(200ms);
    }
    if(!(evicted > last_durable))
        fail("the resent chunks never reached the archive: eviction floor stayed below the last durable event");
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    ++last.logical;
    std::string why = "read never completed";
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto read = client->read(story, {Hlc{}, last});
        if(!read.ok())
        {
            std::this_thread::sleep_for(200ms);
            continue;
        }
        std::vector<Event> events;
        bool complete = false, errored = false;
        for(size_t pull = 0; pull < 256; ++pull)
        {
            auto item = read->next();
            if(!item.ok())
            {
                errored = true;
                break;
            }
            if(!*item)
                break;
            if((**item).completion)
                complete = (**item).completion->complete;
            for(auto& event: (**item).events) events.push_back(std::move(event));
        }
        if(!errored && complete && contains(rows, events, why))
        {
            for(size_t i = 1; i < events.size(); ++i)
                if(events[i - 1].hlc >= events[i].hlc)
                    fail("complete read is not in HLC order");
            if(dump)
            {
                std::ofstream lines(dump);
                for(const auto& event: events)
                    lines << event.id.story_id << ' ' << event.id.writer_id << ' ' << event.id.incarnation << ' '
                          << event.id.sequence << ' ' << event.hlc.physical_ns << ' ' << event.hlc.logical << ' '
                          << event.envelope.payload << '\n';
            }
            std::cout << "verified durable=" << rows.size() << " read=" << events.size()
                      << " evicted_below=" << evicted.physical_ns << '\n';
            return 0;
        }
        if(!errored && complete)
            fail("complete read is wrong: " + why);
        std::this_thread::sleep_for(200ms);
    }
    fail("no complete read: " + why);
}

int after(const char* out)
{
    const auto post = loadHlc(path(out, "post"));
    const auto frontier = loadHlc(path(out, "frontier"));
    Hlc last{};
    for(const auto& row: loadRows(path(out, "events.tsv"))) last = std::max(last, row.hlc);
    if(!(post > frontier))
        fail("first HLC after the restart is not above the frontier reported before the kill");
    if(!(post > last))
        fail("first HLC after the restart is not above every pre-kill event");
    std::cout << "resumed above the reported frontier\n";
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    if(command == "write" && argc == 4)
        return write(argv[2], argv[3]);
    if(command == "frontier" && argc == 4)
        return frontier(argv[2], argv[3]);
    if(command == "append" && argc == 4)
        return append(argv[2], argv[3]);
    if(command == "hot" && argc == 4)
        return hot(argv[2], argv[3]);
    if(command == "verify" && (argc == 5 || argc == 6))
        return verify(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : nullptr);
    if(command == "after" && argc == 3)
        return after(argv[2]);
    std::cerr << "usage: driver write|frontier|append|hot|verify|after ...\n";
    return 2;
}
