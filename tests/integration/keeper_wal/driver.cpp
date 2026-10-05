#include <chrono>
#include <fstream>
#include <filesystem>
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

int checkpoint(const char* keeper, const char* out, bool after)
{
    auto stub = iv1::Archive::NewStub(grpc::CreateChannel(keeper, grpc::InsecureChannelCredentials()));
    const auto rows = loadRows(path(out, "events.tsv"));
    if(rows.empty())
        fail("no checkpoint writers");
    std::ifstream expected(path(out, "checkpoints"));
    std::ofstream saved;
    if(!after)
        saved.open(path(out, "checkpoints"));
    for(const auto& row: rows)
    {
        iv1::WriterStatusRequest req;
        req.set_story_id(row.id.story_id);
        req.set_writer_id(row.id.writer_id);
        req.set_incarnation(row.id.incarnation);
        req.set_sequence(row.id.sequence);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 5s);
        iv1::WriterStatusResponse status;
        if(!stub->WriterStatus(&ctx, req, &status).ok() || !status.known())
            fail("checkpoint writer unknown");
        if(!status.has_recorded_hlc() || fromWire(status.recorded_hlc()) != row.hlc)
            fail("checkpoint lost sequence result");
        if(status.next_sequence() != rows.back().id.sequence + 1 || fromWire(status.last_hlc()) != rows.back().hlc)
            fail("checkpoint lost writer head");
        if(after)
        {
            uint64_t next;
            int64_t physical;
            uint32_t logical;
            bool released;
            int cause;
            if(!(expected >> next >> physical >> logical >> released >> cause) || next != status.next_sequence() ||
               Hlc{physical, logical} != fromWire(status.last_hlc()) || released != status.released() ||
               cause != static_cast<int>(status.termination_cause()))
                fail("checkpoint changed across SIGKILL");
        }
        else
            saved << status.next_sequence() << ' ' << status.last_hlc().physical_ns() << ' '
                  << status.last_hlc().logical() << ' ' << status.released() << ' ' << status.termination_cause()
                  << '\n';
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

// I6.8 across a real Keeper SIGKILL: one complete Read of [0, last) before the kill and the same Read after the
// restart must hold the same DURABLE events (ids, HLCs, payloads). ACCEPTED events may vanish.
std::vector<Event> completeRead(const char* catalog, const char* out, const char* what, uint32_t past_last = 0)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    const auto story = loadStory(out);
    Hlc last{};
    for(const auto& row: loadRows(path(out, "events.tsv"))) last = std::max(last, row.hlc);
    last.logical += past_last;
    std::string why = "read never opened";
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto read = client->read(story, {Hlc{}, last});
        if(read.ok())
        {
            std::vector<Event> events;
            bool complete = false, errored = false;
            for(size_t pull = 0; pull < 256; ++pull)
            {
                auto item = read->next();
                if(!item.ok())
                {
                    errored = true;
                    why = std::string(item.status().message());
                    break;
                }
                if(!*item)
                    break;
                if((**item).completion)
                    complete = (**item).completion->complete;
                for(auto& event: (**item).events) events.push_back(std::move(event));
            }
            if(!errored && complete)
                return events;
            if(!errored)
                why = "read was not complete";
        }
        else
            why = std::string(read.status().message());
        std::this_thread::sleep_for(200ms);
    }
    fail(std::string(what) + ": " + why);
}

struct Stable
{
    Hlc hlc;
    std::string payload;
    bool operator==(const Stable&) const = default;
};

int stableBefore(const char* catalog, const char* out)
{
    const auto events = completeRead(catalog, out, "no complete read before the kill");
    std::ofstream lines(path(out, "stable.tsv"));
    size_t durable_events = 0;
    for(const auto& event: events)
        if(event.durability == Durability::Durable)
        {
            ++durable_events;
            lines << event.id.story_id << ' ' << event.id.writer_id << ' ' << event.id.incarnation << ' '
                  << event.id.sequence << ' ' << event.hlc.physical_ns << ' ' << event.hlc.logical << " 2 2 "
                  << event.envelope.payload << '\n';
        }
    if(!durable_events)
        fail("the complete read before the kill held no DURABLE event");
    std::cout << "stable-before durable=" << durable_events << " read=" << events.size() << '\n';
    return 0;
}

int stableAfter(const char* catalog, const char* out)
{
    std::map<EventId, Stable> before, after;
    for(const auto& row: loadRows(path(out, "stable.tsv"))) before[row.id] = {row.hlc, row.payload};
    if(before.empty())
        fail("no recorded read from before the kill");
    for(const auto& event: completeRead(catalog, out, "no complete read after the restart"))
        if(event.durability == Durability::Durable)
            after[event.id] = {event.hlc, event.envelope.payload};
    if(before != after)
        fail("the DURABLE events of a complete read changed across the Keeper crash: before=" +
             std::to_string(before.size()) + " after=" + std::to_string(after.size()));
    std::cout << "stable-after durable=" << after.size() << '\n';
    return 0;
}

// I8.12 on a real stack: one DURABLE event with a bounded Synced reading, read on both axes once it is archived.
int policyWrite(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok() || !client->createChronicle("policy-marker").ok())
        fail("catalog");
    auto story = client->createStory("policy-marker", "events");
    if(!story.ok())
        fail("createStory");
    std::ofstream(path(out, "story")) << story->id << '\n';
    auto writer = client->acquire(story->id, "writer");
    if(!writer.ok())
        fail("acquire");
    const int64_t stamp =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    sdk::AppendSpec spec;
    spec.envelope.payload = "bounded";
    spec.physical = TimeReading{stamp, 0, ClockStatus::Synced};
    auto result = writer->append(spec);
    if(!result.ok() || !result->acked())
        fail("bounded append: " + (result.ok() ? std::string("not durable") : std::string(result.status().message())));
    std::ofstream(path(out, "stamp")) << stamp << '\n';
    const auto id = result->event_id;
    std::ofstream(path(out, "events.tsv"))
            << id.story_id << ' ' << id.writer_id << ' ' << id.incarnation << ' ' << id.sequence << ' '
            << result->hlc.physical_ns << ' ' << result->hlc.logical << " 2 2 bounded\n";
    auto released = writer->release();
    if(!released.ok() || !*released)
        fail("release");
    return 0;
}

// expect "complete": the physical Read of the event's instant becomes complete. expect "unbounded": the story was
// served by a Keeper without the policy record, so the same Read returns the event and is never complete; it settles
// on PHYSICAL_AXIS_UNBOUNDED while the HLC Read of the archived event is complete.
int policyRead(const char* catalog, const char* keeper, const char* out, const std::string& expect)
{
    const auto rows = durable(out);
    if(rows.size() != 1)
        fail("expected one durable event");
    const auto story = loadStory(out);
    int64_t stamp = 0;
    std::ifstream(path(out, "stamp")) >> stamp;
    const auto deadline = std::chrono::steady_clock::now() + 90s;
    Hlc evicted{};
    while(std::chrono::steady_clock::now() < deadline && !(evicted > rows.front().hlc))
    {
        auto snapshot = fetchHot(keeper, story, 1);
        if(snapshot.ok)
            evicted = snapshot.evicted;
        if(!(evicted > rows.front().hlc))
            std::this_thread::sleep_for(200ms);
    }
    if(!(evicted > rows.front().hlc))
        fail("the event never reached the archive");
    const auto archived = completeRead(catalog, out, "no complete HLC read of the archived event", 1);
    std::string why;
    if(!contains(rows, archived, why))
        fail("HLC read of the archived event: " + why);
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    why = "physical read never opened";
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto read = client->readPhysical(story, {stamp, stamp + 1});
        std::vector<Event> events;
        std::optional<Completion> completion;
        bool errored = !read.ok();
        for(size_t pull = 0; !errored && pull < 256; ++pull)
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
                completion = (**item).completion;
            for(auto& event: (**item).events) events.push_back(std::move(event));
        }
        if(!errored && completion)
        {
            const bool held = contains(rows, events, why) && events.size() == 1;
            if(expect == "unbounded" && completion->complete)
                fail("a physical Read over an unmarked chunk reported complete");
            if(held && expect == "complete" && completion->complete)
            {
                std::cout << "physical read complete\n";
                return 0;
            }
            if(held && expect == "unbounded" && completion->reason == IncompleteReason::PhysicalAxisUnbounded)
            {
                std::cout << "physical read incomplete PHYSICAL_AXIS_UNBOUNDED\n";
                return 0;
            }
            if(held)
                why = "physical read ended with reason " + std::to_string(static_cast<int>(completion->reason));
        }
        std::this_thread::sleep_for(200ms);
    }
    fail("physical read (" + expect + "): " + why);
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

int compactWrite(const char* catalog, const char* out, const char* archive, const char* name)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    if(!client->getChronicle("compaction").ok() && !client->createChronicle("compaction").ok())
        fail("createChronicle");
    auto story = client->createStory("compaction", name);
    if(!story.ok())
        fail("createStory");
    std::ofstream(path(out, "story")) << story->id << '\n';
    auto writer = client->acquire(story->id, "seed");
    if(!writer.ok())
        fail("acquire");
    std::ofstream rows(path(out, "events.tsv"));
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    size_t files = 0;
    for(size_t i = 0; files < 8; ++i)
    {
        if(std::chrono::steady_clock::now() >= deadline)
            fail("eight small archive files never published");
        sdk::AppendSpec spec;
        spec.envelope.payload = "event-" + std::to_string(i);
        auto result = writer->append(spec);
        if(!result.ok() || !result->acked())
            fail("seed append was not acknowledged durable");
        const auto id = result->event_id;
        rows << id.story_id << ' ' << id.writer_id << ' ' << id.incarnation << ' ' << id.sequence << ' '
             << result->hlc.physical_ns << ' ' << result->hlc.logical << " 2 2 " << spec.envelope.payload << '\n';
        rows.flush();
        files = 0;
        std::error_code error;
        for(std::filesystem::directory_iterator it(std::filesystem::path(archive) / std::to_string(story->id), error),
            end;
            !error && it != end;
            it.increment(error))
            files += it->path().extension() == ".h5";
        std::this_thread::sleep_for(50ms);
    }
    auto released = writer->release();
    if(!released.ok() || !*released)
        fail("seed release");
    return 0;
}

void exact(const std::vector<Row>& rows, const std::vector<Event>& events)
{
    std::string why;
    if(events.size() != rows.size() || !contains(rows, events, why))
        fail("exact acknowledged event set: expected=" + std::to_string(rows.size()) +
             " actual=" + std::to_string(events.size()) + " " + why);
}

int compactRead(const char* catalog, const char* keeper, const char* out)
{
    const auto rows = loadRows(path(out, "events.tsv"));
    if(rows.empty())
        fail("empty acknowledged set");
    Hlc last{};
    for(const auto& row: rows) last = std::max(last, row.hlc);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    bool archived = false;
    while(std::chrono::steady_clock::now() < deadline)
    {
        auto hot = fetchHot(keeper, loadStory(out), 1);
        if(hot.ok && hot.evicted > last)
        {
            archived = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    if(!archived)
        fail("acknowledged events did not all leave the Keeper");
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    ++last.logical;
    auto read = client->read(loadStory(out), {Hlc{}, last}, std::chrono::system_clock::now() + 30s);
    if(!read.ok())
        fail("read open: " + std::string(read.status().message()));
    std::vector<Event> events;
    size_t completions = 0;
    for(size_t pulls = 0; pulls < rows.size() + 10; ++pulls)
    {
        auto item = read->next();
        if(!item.ok())
            fail("read stream: " + std::string(item.status().message()));
        if(!*item)
            break;
        if((**item).completion)
        {
            ++completions;
            if(!(**item).completion->complete)
                fail("read Completion was incomplete");
        }
        for(auto& event: (**item).events) events.push_back(std::move(event));
    }
    if(completions != 1)
        fail("read did not return exactly one complete Completion");
    exact(rows, events);
    std::cout << "complete exact read events=" << events.size() << '\n';
    return 0;
}

int compactTail(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    const auto initial = loadRows(path(out, "events.tsv"));
    auto tail = client->tail(loadStory(out), {}, std::chrono::system_clock::now() + 90s);
    if(!tail.ok())
        fail("tail open");
    std::vector<Event> events;
    for(size_t pulls = 0; pulls < 10000; ++pulls)
    {
        auto item = tail->next();
        if(!item.ok() || !*item)
            fail("tail ended before the post-compaction sentinel");
        bool end = false;
        for(auto& event: (**item).events)
        {
            end |= event.envelope.payload == "tail-end";
            events.push_back(std::move(event));
        }
        if(events.size() == initial.size())
        {
            exact(initial, events);
            std::ofstream(path(out, "tail.ready")) << "initial events observed\n";
        }
        if(end)
        {
            exact(loadRows(path(out, "events.tsv")), events);
            tail->cancel();
            std::cout << "exact Tail across switch events=" << events.size() << '\n';
            return 0;
        }
    }
    fail("Tail frame bound exhausted");
}

int compactEnd(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    auto writer = client->acquire(loadStory(out), "sentinel");
    if(!writer.ok())
        fail("sentinel acquire");
    sdk::AppendSpec spec;
    spec.envelope.payload = "tail-end";
    auto result = writer->append(spec);
    if(!result.ok() || !result->acked())
        fail("sentinel append");
    const auto id = result->event_id;
    std::ofstream rows(path(out, "events.tsv"), std::ios::app);
    rows << id.story_id << ' ' << id.writer_id << ' ' << id.incarnation << ' ' << id.sequence << ' '
         << result->hlc.physical_ns << ' ' << result->hlc.logical << " 2 2 tail-end\n";
    rows.close();
    auto released = writer->release();
    if(!released.ok() || !*released)
        fail("sentinel release");
    return 0;
}

int compactDestroy(const char* catalog, const char* out)
{
    auto client = connect(catalog);
    if(!client.ok())
        fail("catalog");
    const auto status = client->destroyStory(loadStory(out));
    if(!status.ok())
        fail("destroy: " + std::string(status.message()));
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string command = argc > 1 ? argv[1] : "";
    if(command == "compact-write" && argc == 6)
        return compactWrite(argv[2], argv[3], argv[4], argv[5]);
    if(command == "compact-read" && argc == 5)
        return compactRead(argv[2], argv[3], argv[4]);
    if(command == "compact-tail" && argc == 4)
        return compactTail(argv[2], argv[3]);
    if(command == "compact-end" && argc == 4)
        return compactEnd(argv[2], argv[3]);
    if(command == "compact-destroy" && argc == 4)
        return compactDestroy(argv[2], argv[3]);
    if(command == "checkpoint-before" && argc == 4)
        return checkpoint(argv[2], argv[3], false);
    if(command == "checkpoint-after" && argc == 4)
        return checkpoint(argv[2], argv[3], true);
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
    if(command == "stable-before" && argc == 4)
        return stableBefore(argv[2], argv[3]);
    if(command == "stable-after" && argc == 4)
        return stableAfter(argv[2], argv[3]);
    if(command == "policy-write" && argc == 4)
        return policyWrite(argv[2], argv[3]);
    if(command == "policy-read" && argc == 6)
        return policyRead(argv[2], argv[3], argv[4], argv[5]);
    if(command == "after" && argc == 3)
        return after(argv[2]);
    std::cerr << "usage: driver write|frontier|append|hot|verify|after ...\n";
    return 2;
}
