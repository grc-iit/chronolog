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

TEST(WalJournal, AdmissionEvidencePrecedesFsyncAndPendingRetriesAddNothing)
{
    WalRig rig;
    rig.control->block();
    std::promise<absl::StatusOr<std::vector<AppendResult>>> first, retry;
    rig.current->appendAsync(batch({1}), Durability::Durable, [&](auto result) { first.set_value(std::move(result)); });
    (void)rig.control->waitPending();
    EXPECT_EQ(rig.current->drainAdmissionEvidence().size(), 1u);
    rig.current->appendAsync(batch({1}), Durability::Durable, [&](auto result) { retry.set_value(std::move(result)); });
    EXPECT_TRUE(rig.current->drainAdmissionEvidence().empty());
    {
        std::lock_guard lock(rig.control->mu);
        rig.control->fail = true;
    }
    rig.control->release();
    auto result = first.get_future().get();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->front().status.code(), absl::StatusCode::kUnavailable);
    EXPECT_TRUE(retry.get_future().get().ok());
    EXPECT_TRUE(rig.current->drainAdmissionEvidence().empty());
}

TEST(WalJournal, TerminationCausesSurviveCheckpointAndReplay)
{
    for(auto cause: {AcquisitionTerminationCause::Expired, AcquisitionTerminationCause::OwnerRemoved})
    {
        WalRig rig;
        auto first = rig.current->append(batch({1}), Durability::Durable);
        ASSERT_TRUE(first.ok());
        ASSERT_TRUE(first->front().status.ok());
        rig.current->releaseWriter(1, 2, 3, cause);
        const Hlc end{first->front().hlc.physical_ns + 1, 0};
        ASSERT_TRUE(rig.current->recordSeal({"cause", 1, first->front().hlc, end, {}, false}).ok());
        // Settling the whole active segment rotates it, which writes the writers checkpoint.
        ASSERT_TRUE(rig.current->recordSettled("cause").ok());
        rig.reopen();
        EXPECT_TRUE(rig.current->drainAdmissionEvidence().empty());
        auto check = [&]
        {
            auto result = rig.current->append(batch({1, 2}), Durability::Accepted);
            ASSERT_TRUE(result.ok());
            for(const auto& item: *result)
                EXPECT_EQ(item.rejection,
                          cause == AcquisitionTerminationCause::Expired ? AppendRejection::FencedExpired
                                                                        : AppendRejection::FencedOwnerRemoved);
        };
        check();
        ASSERT_TRUE(rig.current->registerWriter(1, 2, 4).ok());
        check();
    }
}

TEST(WalJournal, AppendRejectionReasonsReadLegacyWriterCheckpoints)
{
    for(const auto* checkpoint: {"W1\n1 2 3 2 100 1 0 1 1\n1 100 1\n",
                                 "Wv2 1\n1 2 3 2 100 1 0 1 1\n1 100 1 0\n",
                                 "Wv2 1\n1 2 3 2 100 1 0 1 1\n1 0 0 11\n",
                                 "Wv3 1\n1 2 3 2 100 1 0 1 1\n1 100 1 0 0\n"})
    {
        WalRig rig;
        rig.journal.reset();
        const auto path = std::filesystem::path(rig.control->directory) / "1.wal";
        const auto bytes = wal::frame(checkpoint);
        std::ofstream(path, std::ios::binary | std::ios::app).write(bytes.data(), bytes.size());
        rig.reopen();
        auto retry = rig.current->append(batch({1}), Durability::Durable);
        ASSERT_TRUE(retry.ok());
        EXPECT_EQ(retry->front().rejection, AppendRejection::Unspecified);
        EXPECT_EQ(retry->front().status.code(),
                  std::string_view(checkpoint).ends_with("11\n") ? absl::StatusCode::kOutOfRange
                                                                 : absl::StatusCode::kOk);
        EXPECT_TRUE(rig.current->checkpoint().starts_with("v4 "));
    }
}

TEST(WalJournal, AppendRejectionReasonsRejectMalformedCheckpoint)
{
    for(const auto* result: {"1 100 1 0 13\n", "1 100 1 0 3\n", "1 100 1 0\n"})
    {
        WalRig rig;
        rig.journal.reset();
        const auto path = std::filesystem::path(rig.control->directory) / "1.wal";
        const auto bytes = wal::frame(std::string("Wv3 1\n1 2 3 2 100 1 0 1 1\n") + result);
        std::ofstream(path, std::ios::binary | std::ios::app).write(bytes.data(), bytes.size());
        EXPECT_THROW(rig.reopen(), std::runtime_error);
    }
}

TEST(WalJournal, TerminationCausesRejectMalformedCheckpoint)
{
    // Out of range, on an unreleased writer, and missing.
    for(const auto* writer: {"1 2 3 2 100 1 1 1 0 5\n", "1 2 3 2 100 1 0 1 0 1\n", "1 2 3 2 100 1 1 1 0\n"})
    {
        WalRig rig;
        rig.journal.reset();
        const auto path = std::filesystem::path(rig.control->directory) / "1.wal";
        const auto bytes = wal::frame(std::string("Wv4 1\n") + writer);
        std::ofstream(path, std::ios::binary | std::ios::app).write(bytes.data(), bytes.size());
        EXPECT_THROW(rig.reopen(), std::runtime_error);
    }
}

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

// The window lines of the one writer in a "v4" checkpoint, by sequence.
std::map<uint64_t, std::pair<int64_t, uint32_t>> checkpointWindow(const std::string& text, size_t* declared = nullptr)
{
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "v4 1");
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
    int code, rejection;
    while(in >> sequence >> physical >> logical >> code >> rejection)
    {
        EXPECT_EQ(code, 0);
        EXPECT_EQ(rejection, 0);
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
    for(uint64_t writer = 10; writer < 15; ++writer)
    {
        ASSERT_TRUE(rig.current->registerWriter(1, writer, 3).ok());
        AppendBatch own{1, 7, {}};
        AppendItem item;
        item.writer_id = writer;
        item.incarnation = 3;
        item.sequence = 1;
        item.envelope.payload = "writer " + std::to_string(writer);
        own.items.push_back(std::move(item));
        queued.push_back(std::async(std::launch::async,
                                    [&rig, own = std::move(own)]
                                    { return rig.current->append(own, Durability::Durable); }));
    }
    const auto give_up = std::chrono::steady_clock::now() + 10s;
    while(rig.current->queuedRecords() < queued.size() && std::chrono::steady_clock::now() < give_up)
        std::this_thread::yield();
    ASSERT_EQ(rig.current->queuedRecords(), queued.size());
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

TEST(WalJournal, WindowedGroupReportsDurableOnlyAfterSyncReturns)
{
    WalRig rig(64ull << 20, 65536, 2000);
    const auto before = rig.current->commitStats();
    size_t syncs_before;
    {
        std::lock_guard lock(rig.control->mu);
        syncs_before = rig.control->syncs;
    }
    rig.control->block();
    auto append =
            std::async(std::launch::async, [&] { return rig.current->append(batch({1, 2}), Durability::Durable); });
    (void)rig.control->waitPending();
    {
        std::unique_lock lock(rig.control->mu);
        ASSERT_TRUE(rig.control->cv.wait_for(lock, 5s, [&] { return rig.control->syncs > syncs_before; }));
    }
    // The group has left the window and sits in a blocked sync: nothing may be acknowledged yet.
    EXPECT_EQ(append.wait_for(50ms), std::future_status::timeout);
    EXPECT_EQ(rig.current->commitStats().syncs, before.syncs);
    rig.control->release();
    ASSERT_EQ(append.wait_for(5s), std::future_status::ready);
    auto results = append.get();
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 2u);
    for(const auto& result: *results)
    {
        EXPECT_TRUE(result.status.ok());
        EXPECT_EQ(result.achieved, Durability::Durable);
    }
    const auto after = rig.current->commitStats();
    EXPECT_EQ(after.syncs, before.syncs + 1);
    EXPECT_EQ(after.records, before.records + 2);
}

TEST(WalJournal, GroupCommitWindowAboveTheBoundIsRefused)
{
    WalRig rig;
    auto config = rig.config;
    config.wal_dir = rig.control->directory + "/oversized";
    config.group_commit_window_us = kMaxGroupCommitWindowUs + 1;
    EXPECT_THROW(WalJournal(rig.clock, rig.membership, rig.ram_config, config), std::invalid_argument);
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
    WorkerPool pool(1, 8);
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


// I13.16: below wal_reserve_bytes of free space new appends are refused CAPACITY before the WAL can fail for space, and
// admission resumes once the committer samples the reserve restored, with no append to wake it.
TEST(WalJournal, WalReserveRefusesBeforeTheWalFails)
{
    WalRig rig;
    auto free = std::make_shared<std::atomic<uint64_t>>(1ull << 30);
    rig.config.wal_reserve_bytes = 64ull << 20;
    rig.config.free_bytes = [free](const std::string&) -> absl::StatusOr<uint64_t> { return free->load(); };
    rig.reopen();
    auto admitted = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(admitted.ok());
    ASSERT_TRUE(admitted->front().status.ok()) << admitted->front().status;
    free->store(1ull << 20);
    uint64_t next = 2;
    std::optional<AppendResult> refused;
    for(const auto until = std::chrono::steady_clock::now() + 10s; !refused && std::chrono::steady_clock::now() < until;)
    {
        auto result = rig.current->append(batch({next}), Durability::Durable);
        ASSERT_TRUE(result.ok());
        if(result->front().rejection == AppendRejection::Capacity)
            refused = result->front();
        else
        {
            ASSERT_TRUE(result->front().status.ok()) << result->front().status;
            ++next;
            std::this_thread::sleep_for(10ms);
        }
    }
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status.code(), absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(refused->achieved, Durability::Unspecified);
    auto duplicate = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_EQ(duplicate->front().status, admitted->front().status);
    EXPECT_EQ(duplicate->front().hlc, admitted->front().hlc);
    free->store(1ull << 30);
    std::optional<AppendResult> resumed;
    for(const auto until = std::chrono::steady_clock::now() + 10s; !resumed && std::chrono::steady_clock::now() < until;)
    {
        auto result = rig.current->append(batch({next}), Durability::Durable);
        ASSERT_TRUE(result.ok());
        if(result->front().rejection == AppendRejection::Capacity)
            std::this_thread::sleep_for(10ms);
        else
            resumed = result->front();
    }
    ASSERT_TRUE(resumed);
    EXPECT_TRUE(resumed->status.ok()) << resumed->status;
    EXPECT_EQ(resumed->achieved, Durability::Durable);
    EXPECT_EQ(resumed->id.sequence, next);
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

namespace chronolog::test
{
namespace
{
std::map<std::string, std::string> walFiles(const std::string& directory)
{
    std::map<std::string, std::string> files;
    for(const auto& entry: std::filesystem::directory_iterator(directory))
    {
        std::ifstream stream(entry.path(), std::ios::binary);
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        files[entry.path().filename().string()] = bytes.str();
    }
    return files;
}

std::string refusal(WalRig& rig)
{
    try
    {
        rig.reopen();
    }
    catch(const std::runtime_error& error)
    {
        return error.what();
    }
    return {};
}
} // namespace

TEST(WalJournal, WalStampedByAnotherDeploymentRefusesAndLeavesFilesUntouched)
{
    WalRig rig;
    rig.config.deployment_id = "deployment-a";
    rig.reopen();
    auto appended = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok());
    rig.journal.reset();
    const auto before = walFiles(rig.control->directory);
    ASSERT_EQ(before.at("deployment"), "deployment-a");

    rig.config.deployment_id = "deployment-b";
    const auto message = refusal(rig);
    EXPECT_NE(message.find("deployment-a"), std::string::npos) << message;
    EXPECT_NE(message.find("deployment-b"), std::string::npos) << message;
    EXPECT_NE(message.find(std::filesystem::absolute(rig.control->directory).string()), std::string::npos) << message;
    EXPECT_EQ(walFiles(rig.control->directory), before);

    rig.config.deployment_id = "deployment-a";
    rig.reopen();
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().envelope.payload, "event 1");
}

TEST(WalJournal, UnstampedWalIsAdoptedAndStampedOnFirstUse)
{
    WalRig rig;
    auto appended = rig.current->append(batch({1}), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok());
    rig.journal.reset();
    ASSERT_FALSE(walFiles(rig.control->directory).contains("deployment"));

    rig.config.deployment_id = "deployment-a";
    rig.reopen();
    auto events = rig.current->read(1, all());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
    rig.journal.reset();
    const auto stamped = walFiles(rig.control->directory);
    EXPECT_EQ(stamped.at("deployment"), "deployment-a");
    EXPECT_FALSE(stamped.contains("deployment.tmp"));

    rig.config.deployment_id = "deployment-b";
    EXPECT_NE(refusal(rig).find("belongs to deployment 'deployment-a'"), std::string::npos);
}
} // namespace chronolog::test
