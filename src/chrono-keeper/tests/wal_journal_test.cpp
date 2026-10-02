#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <sstream>

#include "adapter/JournalService.h"
#include "wal_harness.h"

namespace chronolog::test
{
namespace
{
using namespace std::chrono_literals;

AppendBatch batch(std::initializer_list<uint64_t> sequences)
{
    AppendBatch out{1, 7, {}};
    for(auto sequence: sequences)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "event " + std::to_string(sequence);
        out.items.push_back(std::move(item));
    }
    return out;
}
Range all() { return {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}}; }

TEST(WalJournal, RecoveredUnsettledSealRequiresArchiveEvenWhenAcceptedEventsAreGone)
{
    WalRig rig;
    auto accepted = rig.current->append(batch({1}), Durability::Accepted);
    auto durable = rig.current->append(batch({2}), Durability::Durable);
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(durable.ok());
    ASSERT_TRUE(accepted->front().status.ok());
    ASSERT_TRUE(durable->front().status.ok());
    const Hlc end{durable->front().hlc.physical_ns + 1, 0};
    ASSERT_TRUE(rig.current->recordSeal({"unsettled", 1, accepted->front().hlc, end, {}, false}).ok());
    EXPECT_EQ(rig.current->evictionFloor(1), Hlc{});
    rig.reopen();
    EXPECT_EQ(rig.current->evictionFloor(1), end);
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().id.sequence, 2u);
    ASSERT_EQ(rig.current->sealedChunks().size(), 1u);
    EXPECT_FALSE(rig.current->sealedChunks().front().settled);
}

TEST(WalJournal, TornTailRecoveryPreservesDurableEvents)
{
    WalRig rig;
    auto appended = rig.current->append(batch({1, 2}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE((*appended)[1].status.ok());
    rig.journal.reset();
    const auto path = std::filesystem::path(rig.control->directory) / "1.wal";
    Event tail;
    tail.id = {1, 2, 3, 3};
    tail.hlc = {100, 999};
    auto bytes = wal::frame(wal::encode(tail));
    bytes.resize(bytes.size() - 2);
    std::ofstream(path, std::ios::binary | std::ios::app).write(bytes.data(), bytes.size());
    rig.reopen();
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 2u);
    for(size_t i = 0; i < events->size(); ++i) EXPECT_EQ((*events)[i].hlc, (*appended)[i].hlc);
    auto next = rig.current->append(batch({3}), Durability::Durable);
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE((*next)[0].status.ok());
}

// The window lines of the one writer in a "v2" checkpoint, by sequence.
std::map<uint64_t, std::pair<int64_t, uint32_t>> checkpointWindow(const std::string& text, size_t* declared = nullptr)
{
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "v2 1");
    std::getline(in, line);
    std::istringstream header(line);
    uint64_t story, writer, incarnation, next, released, assigned;
    int64_t physical;
    uint32_t logical;
    size_t count;
    header >> story >> writer >> incarnation >> next >> physical >> logical >> released >> assigned >> count;
    if(declared)
        *declared = count;
    std::map<uint64_t, std::pair<int64_t, uint32_t>> window;
    uint64_t sequence;
    int code;
    while(in >> sequence >> physical >> logical >> code)
    {
        EXPECT_EQ(code, 0);
        EXPECT_TRUE(window.emplace(sequence, std::pair{physical, logical}).second)
                << "sequence " << sequence << " twice";
    }
    return window;
}

TEST(WalJournal, AppendsQueuedBehindAnFsyncAreAcknowledgedByTheNextSingleFsync)
{
    WalRig rig;
    rig.control->block();
    auto first = std::async(std::launch::async, [&] { return rig.current->append(batch({1}), Durability::Durable); });
    (void)rig.control->waitPending();
    std::vector<std::future<absl::StatusOr<std::vector<AppendResult>>>> queued;
    for(uint64_t sequence = 2; sequence <= 6; ++sequence)
        queued.push_back(std::async(std::launch::async,
                                    [&, sequence]
                                    { return rig.current->append(batch({sequence}), Durability::Durable); }));
    std::this_thread::sleep_for(300ms);
    size_t before;
    {
        std::lock_guard lock(rig.control->mu);
        before = rig.control->syncs;
    }
    rig.control->release();
    ASSERT_EQ(first.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(first.get()->front().status.ok());
    for(auto& call: queued)
    {
        ASSERT_EQ(call.wait_for(5s), std::future_status::ready);
        auto results = call.get();
        ASSERT_TRUE(results.ok());
        EXPECT_TRUE(results->front().status.ok());
        EXPECT_EQ(results->front().achieved, Durability::Durable);
    }
    std::lock_guard lock(rig.control->mu);
    // The group that was already syncing, then one group for everything that queued behind it.
    EXPECT_LE(rig.control->syncs - before, 1u);
}

TEST(WalJournal, CheckpointCacheMatchesTheWindowAcrossBlocksTrimsAndUpgrades)
{
    WalRig rig(256 * 1024);
    rig.ram_config.dedupe_window = 1500;
    rig.reopen();
    std::map<uint64_t, Hlc> hlcs;
    for(uint64_t from = 1; from <= 4000; from += 500)
    {
        AppendBatch many{1, 7, {}};
        for(uint64_t sequence = from; sequence < from + 500; ++sequence)
            many.items.push_back(batch({sequence}).items.front());
        auto appended = rig.current->append(many, Durability::Durable);
        ASSERT_TRUE(appended.ok());
        for(const auto& result: *appended)
        {
            ASSERT_TRUE(result.status.ok());
            hlcs[result.id.sequence] = result.hlc;
        }
        size_t declared = 0;
        const auto window = checkpointWindow(rig.current->checkpoint(), &declared);
        EXPECT_EQ(declared, window.size());
        const uint64_t last = from + 499, low = last > 1500 ? last - 1500 + 1 : 1;
        for(uint64_t sequence = low; sequence <= last; ++sequence)
        {
            ASSERT_TRUE(window.contains(sequence)) << "sequence " << sequence << " missing after " << last;
            EXPECT_EQ(window.at(sequence), std::pair(hlcs[sequence].physical_ns, hlcs[sequence].logical));
        }
        // Whole cache blocks of 1024 may extend below the window, never above it and never without a result.
        ASSERT_FALSE(window.empty());
        EXPECT_EQ(window.rbegin()->first, last);
        EXPECT_GE(window.begin()->first + 1024 + 1500, last);
    }
    // A retry inside the window returns the original result after a restart; one outside it is refused.
    rig.reopen();
    auto retry = rig.current->append(batch({3990}), Durability::Durable);
    ASSERT_TRUE(retry.ok());
    EXPECT_TRUE(retry->front().status.ok());
    EXPECT_EQ(retry->front().hlc, hlcs[3990]);
    auto outside = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(outside.ok());
    EXPECT_EQ(outside->front().status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(WalJournal, CheckpointCacheSkipsAcceptedEntriesUntilARetryMakesThemDurable)
{
    WalRig rig;
    ASSERT_TRUE(rig.current->append(batch({1, 2, 3}), Durability::Accepted).ok());
    auto durable = rig.current->append(batch({4, 5}), Durability::Durable);
    ASSERT_TRUE(durable.ok());
    auto window = checkpointWindow(rig.current->checkpoint());
    EXPECT_EQ(window.size(), 2u);
    EXPECT_TRUE(window.contains(4) && window.contains(5));
    // The first checkpoint cached everything below the newest entries; upgrading entry 2 must reach the next one.
    auto upgraded = rig.current->append(batch({2}), Durability::Durable);
    ASSERT_TRUE(upgraded.ok());
    ASSERT_TRUE(upgraded->front().status.ok());
    EXPECT_EQ(upgraded->front().achieved, Durability::Durable);
    window = checkpointWindow(rig.current->checkpoint());
    EXPECT_EQ(window.size(), 3u);
    ASSERT_TRUE(window.contains(2));
    EXPECT_EQ(window.at(2), std::pair(upgraded->front().hlc.physical_ns, upgraded->front().hlc.logical));
}

TEST(WalJournal, ReleasedWriterKeepsItsCountersButNotItsWindowInTheCheckpoint)
{
    WalRig rig;
    auto appended = rig.current->append(batch({1, 2, 3}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    size_t declared = 0;
    EXPECT_EQ(checkpointWindow(rig.current->checkpoint(), &declared).size(), 3u);
    rig.current->releaseWriter(1, 2, 3);
    const auto window = checkpointWindow(rig.current->checkpoint(), &declared);
    EXPECT_TRUE(window.empty());
    EXPECT_EQ(declared, 0u);
    auto retry = rig.current->append(batch({3}), Durability::Durable);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->front().status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(WalJournal, SettlingRotatesOnlyWhenTheWholeActiveSegmentCanBeFreed)
{
    WalRig rig;
    const auto segments = [&]
    {
        std::set<uint64_t> numbers;
        for(const auto& entry: std::filesystem::directory_iterator(rig.control->directory))
            if(entry.path().extension() == ".wal")
                numbers.insert(std::stoull(entry.path().stem().string()));
        return numbers;
    };
    const auto append = [&](uint64_t first, uint64_t last)
    {
        std::vector<Hlc> hlcs;
        for(uint64_t sequence = first; sequence <= last; ++sequence)
        {
            auto result = rig.current->append(batch({sequence}), Durability::Durable);
            EXPECT_TRUE(result.ok() && result->front().status.ok());
            hlcs.push_back(result->front().hlc);
        }
        return hlcs;
    };
    const auto settle = [&](const std::string& id, Hlc start, Hlc last)
    {
        ASSERT_TRUE(rig.current->recordSeal({id, 1, start, {last.physical_ns, last.logical + 1}, {}, false}).ok());
        ASSERT_TRUE(rig.current->recordSettled(id).ok());
    };
    const auto first_segment = *segments().begin();
    const auto ten = append(1, 10);
    // Every event of the active segment is settled: it is rotated away, leaving one new segment.
    settle("all", ten.front(), ten.back());
    ASSERT_EQ(segments().size(), 1u);
    EXPECT_EQ(*segments().begin(), first_segment + 1);
    const auto more = append(11, 20);
    // Half of the new active segment is still unsettled: nothing is rotated and nothing can be removed yet.
    settle("half", more.front(), more[4]);
    EXPECT_EQ(segments(), std::set<uint64_t>{first_segment + 1});
    // The rest settles: the active segment goes the same way as the first one.
    settle("rest", more[5], more.back());
    EXPECT_EQ(segments(), std::set<uint64_t>{first_segment + 2});
}

TEST(WalJournal, DurableRpcDoesNotOccupyTheWorkerDuringFsync)
{
    WalRig rig;
    keeper::WorkerPool pool(1, 8);
    keeper::JournalService service(*rig.current, pool);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = v1::Journal::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    auto call = [&](Durability durability, uint64_t sequence)
    {
        v1::AppendRequest request;
        request.set_story_id(1);
        request.set_epoch(7);
        request.set_durability(durability == Durability::Durable ? v1::DURABILITY_DURABLE : v1::DURABILITY_ACCEPTED);
        auto* item = request.add_items();
        item->set_writer_id(2);
        item->set_incarnation(3);
        item->set_sequence(sequence);
        item->mutable_physical()->set_physical_ns(rig.clock->now()->physical_ns);
        item->mutable_physical()->set_status(v1::CLOCK_STATUS_UNSYNCED);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        v1::AppendResponse response;
        EXPECT_TRUE(stub->Append(&context, request, &response).ok());
        return response;
    };
    rig.control->block();
    auto durable = std::async(std::launch::async, [&] { return call(Durability::Durable, 1); });
    (void)rig.control->waitPending();
    auto accepted_call = std::async(std::launch::async, [&] { return call(Durability::Accepted, 2); });
    std::promise<void> worker_ran;
    auto worker_ready = worker_ran.get_future();
    EXPECT_TRUE(pool.submit([&] { worker_ran.set_value(); }));
    EXPECT_EQ(worker_ready.wait_for(1s), std::future_status::ready);
    rig.control->release();
    ASSERT_EQ(accepted_call.wait_for(5s), std::future_status::ready);
    auto accepted = accepted_call.get();
    ASSERT_EQ(accepted.results_size(), 1);
    EXPECT_EQ(accepted.results(0).status().code(), 0);
    EXPECT_EQ(accepted.results(0).achieved_durability(), v1::DURABILITY_ACCEPTED);
    ASSERT_EQ(durable.wait_for(5s), std::future_status::ready);
    auto response = durable.get();
    ASSERT_EQ(response.results_size(), 1);
    EXPECT_EQ(response.results(0).achieved_durability(), v1::DURABILITY_DURABLE);
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

} // namespace
} // namespace chronolog::test

namespace chronolog::test
{
TEST(WalJournal, AcceptedThenDurableUpgradeSurvivesCrashWithOutOfOrderSequences)
{
    WalRig rig;
    AppendBatch batch{1, 7, {}};
    for(uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "original";
        batch.items.push_back(item);
    }
    auto accepted = rig.current->append(batch, Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    for(int index: {2, 0, 1})
    {
        auto item = batch.items[index];
        item.envelope.payload = "retry must not replace payload";
        auto durable = rig.current->append({1, 7, {item}}, index == 0 ? Durability::Unspecified : Durability::Durable);
        ASSERT_TRUE(durable.ok());
        ASSERT_TRUE(durable->front().status.ok());
        EXPECT_EQ(durable->front().hlc, (*accepted)[index].hlc);
        EXPECT_EQ(durable->front().achieved, Durability::Durable);
    }
    rig.reopen();
    auto replay = rig.current->append(batch, Durability::Durable);
    ASSERT_TRUE(replay.ok());
    for(size_t i = 0; i < replay->size(); ++i)
    {
        EXPECT_TRUE((*replay)[i].status.ok());
        EXPECT_EQ((*replay)[i].hlc, (*accepted)[i].hlc);
    }
    auto events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 3u);
    for(const auto& event: *events) EXPECT_EQ(event.envelope.payload, "original");
    auto item = batch.items.front();
    item.sequence = 4;
    auto next = rig.current->append({1, 7, {item}}, Durability::Durable);
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE(next->front().status.ok());
}
} // namespace chronolog::test

namespace chronolog::test
{
TEST(WalJournal, PendingUpgradeKeepsTheOriginalVisibleAndMakesRetriesWait)
{
    WalRig rig;
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    item.envelope.payload = "original";
    auto original = rig.current->append({1, 7, {item}}, Durability::Accepted);
    ASSERT_TRUE(original.ok());
    ASSERT_TRUE(original->front().status.ok());
    rig.control->block();
    auto upgrade =
            std::async(std::launch::async, [&] { return rig.current->append({1, 7, {item}}, Durability::Durable); });
    (void)rig.control->waitPending();
    auto retry =
            std::async(std::launch::async, [&] { return rig.current->append({1, 7, {item}}, Durability::Accepted); });
    EXPECT_EQ(retry.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    auto events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    EXPECT_TRUE(events.ok());
    if(events.ok() && events->size() == 1)
    {
        EXPECT_EQ(events->front().durability, Durability::Accepted);
    }
    rig.control->release();
    ASSERT_EQ(upgrade.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(retry.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    for(auto* future: {&upgrade, &retry})
    {
        auto result = future->get();
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(result->front().status.ok());
        EXPECT_EQ(result->front().hlc, original->front().hlc);
        EXPECT_EQ(result->front().achieved, Durability::Durable);
    }
    events = rig.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().durability, Durability::Durable);
}
} // namespace chronolog::test

namespace chronolog::test
{
TEST(WalJournal, RotationPreservesReleasedWritersAndUnsettledSealIdentity)
{
    WalRig rig;
    rig.config.wal_segment_bytes = 1;
    rig.reopen();
    auto appended = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok());
    Chunk chunk;
    chunk.id = "keeper:1:0.0";
    chunk.story_id = 1;
    chunk.start = {};
    chunk.end = {1000, 0};
    ASSERT_TRUE(rig.current->recordSeal(chunk).ok());
    rig.current->releaseWriter(1, 2, 3);
    ASSERT_TRUE(rig.current->recordSettled(chunk.id).ok());
    ASSERT_TRUE(rig.current->registerWriter(1, 4, 1).ok());
    rig.clock->setPhysical(1100);
    auto next_batch = batch({1});
    next_batch.items.front().writer_id = 4;
    next_batch.items.front().incarnation = 1;
    auto next = rig.current->append(next_batch, Durability::Durable);
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(next->front().status.ok());
    chunk.id = "keeper:1:1000.0";
    chunk.start = chunk.end;
    chunk.end = {2'000'000'000, 0};
    ASSERT_TRUE(rig.current->recordSeal(chunk).ok());
    rig.reopen();
    auto released = rig.current->append(batch({2}), Durability::Durable);
    ASSERT_TRUE(released.ok());
    EXPECT_EQ(released->front().status.code(), absl::StatusCode::kFailedPrecondition);
    const auto seals = rig.current->sealedChunks();
    ASSERT_EQ(seals.size(), 2u);
    auto seal = std::find_if(seals.begin(), seals.end(), [&](const auto& value) { return value.chunk.id == chunk.id; });
    ASSERT_NE(seal, seals.end());
    EXPECT_FALSE(seal->settled);
    EXPECT_EQ(seal->chunk.start, chunk.start);
    EXPECT_EQ(seal->chunk.end, chunk.end);
    EXPECT_GT(rig.clock->tick(), next->front().hlc);
}
} // namespace chronolog::test

namespace chronolog
{
TEST(WalJournal, RecoveredInstanceAndFirstEventSurviveRotation)
{
    test::WalRig rig(4096);
    ASSERT_TRUE(rig.current->recordInstance("old").ok());
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    item.physical = {100, 1, ClockStatus::Synced};
    item.envelope.payload.assign(8192, 'x');
    auto result = rig.current->append({1, 7, {item}}, Durability::Durable);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok());
    const auto first = result->front().hlc;
    rig.reopen();
    EXPECT_EQ(rig.current->recoveredInstance(), "old");
    ASSERT_TRUE(rig.current->firstEvent(1));
    EXPECT_EQ(*rig.current->firstEvent(1), first);
    ASSERT_TRUE(rig.current->recordInstance("replacement").ok());
    rig.reopen();
    EXPECT_EQ(rig.current->recoveredInstance(), "replacement");
    EXPECT_EQ(*rig.current->firstEvent(1), first);
}
} // namespace chronolog
