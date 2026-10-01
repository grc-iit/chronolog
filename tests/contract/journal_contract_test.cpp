#include <future>
#include <atomic>
#include <set>
#include <thread>
// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/journal.h"
namespace chronolog::contract
{
// Seed story=1 epoch=7, registered writer=2 incarnation=3. Clock starts at 100.
// supports_durable selects a WAL implementation or the ACCEPTED-only skeleton.
// crashRestart loses RAM and reopens WAL; setPhysical preserves HLC state.
struct JournalHarness
{
    std::unique_ptr<Journal> sut;
    size_t payload_limit{1024 * 1024};
    int64_t causal_skew_limit_ns{1000};
    std::function<void()> supersedeIncarnation;
    std::function<void()> unassignWriter;
    bool supports_durable{};
    std::function<void()> crashRestart;
    std::function<void(int64_t)> setPhysical;
    // Apply and confirm the Keeper release fence before returning.
    std::function<void()> releaseIncarnation;
    // Add an idle registered incarnation to this same Keeper without an event.
    std::function<void(uint64_t, uint64_t)> registerIdleWriter;
    // blockFsync blocks the next WAL sync; waitPendingHlc waits until assigned;
    // releaseFsync unblocks sync. Controls do not mutate frontier semantics.
    std::function<void()> blockFsync;
    std::function<Hlc()> waitPendingHlc;
    std::function<void()> releaseFsync;
    // Inject a WAL fsync error without changing append results.
    std::function<void(bool)> failFsync;
    // Pause after clock assignment while the writer critical section is active.
    std::function<void(std::function<void(Hlc)>)> onAssignment;
    // Pause after one writer was scanned, before returning the snapshot.
    std::function<void(std::function<void()>)> onWriterScanned;
    // Return events and their exclusive seal from the same scan.
    std::function<absl::StatusOr<std::pair<Hlc, std::vector<Event>>>()> snapshot;
    // Seal archive windows using the injected clock and return retained chunks.
    std::function<absl::StatusOr<std::vector<Chunk>>()> sealArchive;
    // Deliver a validated receipt from the chosen Grapher instance.
    std::function<void(const Chunk&, std::string, uint64_t)> deliverChunk;
    // Apply a Grapher watermark and receipt report.
    std::function<void(WatermarkReport)> reportArchive;
    // Release the story's tail retention reference.
    std::function<void()> releaseTail;
    // Inspect durable segment identities without changing WAL state.
    std::function<std::set<std::string>()> walSegments;
    // Observe the recovered eviction floor.
    std::function<Hlc()> evictionFloor;
};
AppendItem Item(uint64_t sequence = 1)
{
    AppendItem i;
    i.writer_id = 2;
    i.incarnation = 3;
    i.sequence = sequence;
    i.physical = {100, 1, ClockStatus::Synced};
    i.envelope.payload = "event";
    return i;
}
AppendBatch Batch(std::vector<AppendItem> items, Epoch epoch = 7) { return {1, epoch, std::move(items)}; }
EventId Identity(const AppendItem& item) { return {1, item.writer_id, item.incarnation, item.sequence}; }
Range All() { return {Range::Axis::Hlc, {0, 0}, {INT64_MAX, UINT32_MAX}}; }

using JournalFactory = std::function<std::unique_ptr<JournalHarness>()>;
class JournalContract: public ::testing::TestWithParam<JournalFactory>
{
protected:
    std::unique_ptr<JournalHarness> h;
    absl::StatusOr<Chunk> archiveChunk(uint64_t sequence = 1)
    {
        h->setPhysical(static_cast<int64_t>(sequence) * 1'000'000'000 - 900'000'000);
        auto item = Item(sequence);
        item.envelope.payload.assign(8192, 'x');
        auto appended = h->sut->append(Batch({item}), Durability::Durable);
        if(!appended.ok())
            return appended.status();
        if(!appended->at(0).status.ok())
            return appended->at(0).status;
        h->setPhysical(static_cast<int64_t>(sequence) * 1'000'000'000);
        auto chunks = h->sealArchive();
        if(!chunks.ok())
            return chunks.status();
        if(chunks->empty())
            return absl::InternalError("seal returned no chunks");
        return chunks->back();
    }
    size_t eventCount()
    {
        auto events = h->sut->read(1, All());
        EXPECT_TRUE(events.ok());
        return events.ok() ? events->size() : SIZE_MAX;
    }
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
    }
};

TEST_P(JournalContract, IdempotentRetryReturnsOriginalResult)
{
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    auto b = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_EQ(b->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    EXPECT_TRUE((*b)[0].status.ok());
    EXPECT_EQ((*a)[0].hlc, (*b)[0].hlc);
    EXPECT_EQ((*a)[0].id, (*b)[0].id);
    auto events = h->sut->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
}

TEST_P(JournalContract, GaplessSequenceRejection)
{
    auto a = h->sut->append(Batch({Item(2)}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    EXPECT_EQ((*a)[0].status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_NE((*a)[0].status.message().find("1"), std::string::npos);
    auto b = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->size(), 1u);
    EXPECT_TRUE((*b)[0].status.ok());
}

TEST_P(JournalContract, NoSilentDurabilityDowngrade)
{
    for(auto d: {Durability::Unspecified, Durability::Durable})
    {
        auto a = h->sut->append(Batch({Item()}), d);
        ASSERT_TRUE(a.ok());
        ASSERT_EQ(a->size(), 1u);
        if(h->supports_durable)
        {
            EXPECT_TRUE((*a)[0].status.ok());
            EXPECT_EQ((*a)[0].achieved, Durability::Durable);
        }
        else
        {
            EXPECT_EQ((*a)[0].status.code(), absl::StatusCode::kUnimplemented);
            EXPECT_EQ((*a)[0].achieved, Durability::Unspecified);
        }
    }
}

TEST_P(JournalContract, AcceptedIsExplicitRamReceipt)
{
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    EXPECT_TRUE((*a)[0].status.ok());
    EXPECT_EQ((*a)[0].achieved, Durability::Accepted);
}

TEST_P(JournalContract, DurableAckSurvivesCrash)
{
    ASSERT_TRUE(h->crashRestart);
    auto a = h->sut->append(Batch({Item()}), Durability::Durable);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    if(!h->supports_durable)
    {
        EXPECT_FALSE((*a)[0].status.ok());
        return;
    }
    ASSERT_TRUE((*a)[0].status.ok());
    h->crashRestart();
    auto e = h->sut->read(1, All());
    ASSERT_TRUE(e.ok());
    ASSERT_EQ(e->size(), 1u);
    EXPECT_EQ((*e)[0].id, Identity(Item()));
}

TEST_P(JournalContract, StaleEpochReturnsCurrentRoute)
{
    auto i = Item();
    auto a = h->sut->append(Batch({i}, 6), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    EXPECT_EQ((*a)[0].status.code(), absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE((*a)[0].current_route);
    EXPECT_EQ((*a)[0].current_route->epoch, 7u);
}

TEST_P(JournalContract, KeeperAssignsHlcAboveCausalFloor)
{
    auto i = Item();
    i.causal_floor = {500, 8};
    auto a = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    EXPECT_GT((*a)[0].hlc, i.causal_floor);
}

TEST_P(JournalContract, PerWriterOrderSurvivesBackwardPhysicalStep)
{
    ASSERT_TRUE(h->setPhysical);
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    h->setPhysical(50);
    auto b = h->sut->append(Batch({Item(2)}), Durability::Accepted);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->size(), 1u);
    ASSERT_TRUE((*b)[0].status.ok());
    EXPECT_GT((*b)[0].hlc, (*a)[0].hlc);
}

TEST_P(JournalContract, PayloadAndTraceContextValidation)
{
    auto i = Item();
    i.envelope.payload.assign(h->payload_limit + 1, 'x');
    auto a = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    EXPECT_EQ((*a)[0].status.code(), absl::StatusCode::kInvalidArgument);
    i = Item();
    i.envelope.trace_id = "short";
    auto b = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->size(), 1u);
    EXPECT_EQ((*b)[0].status.code(), absl::StatusCode::kInvalidArgument);
}

TEST_P(JournalContract, HalfOpenRangeAndFrontierIncludesRegisteredWriter)
{
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    Range r{Range::Axis::Hlc, {0, 0}, (*a)[0].hlc};
    auto e = h->sut->read(1, r);
    ASSERT_TRUE(e.ok());
    EXPECT_TRUE(e->empty());
    auto f = h->sut->frontier(1);
    ASSERT_TRUE(f.ok());
    ASSERT_EQ(f->size(), 1u);
    EXPECT_EQ((*f)[0].writer_id, 2u);
    EXPECT_EQ((*f)[0].incarnation, 3u);
}

TEST_P(JournalContract, EnvelopeAndPhysicalReadingRoundTrip)
{
    auto i = Item();
    i.envelope.content_type = "application/json";
    i.envelope.trace_id.assign(16, 't');
    i.envelope.span_id.assign(8, 's');
    i.envelope.attributes["gen_ai.agent.id"] = "agent";
    auto a = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    auto e = h->sut->read(1, All());
    ASSERT_TRUE(e.ok());
    ASSERT_EQ(e->size(), 1u);
    EXPECT_EQ((*e)[0].envelope.attributes, i.envelope.attributes);
    EXPECT_EQ((*e)[0].envelope.trace_id, i.envelope.trace_id);
    EXPECT_EQ((*e)[0].envelope.span_id, i.envelope.span_id);
    EXPECT_EQ((*e)[0].envelope.payload, i.envelope.payload);
    EXPECT_EQ((*e)[0].physical.physical_ns, 100);
}

TEST_P(JournalContract, PerItemFailureDoesNotEraseSuccessfulBatchItems)
{
    auto stale = Item(2);
    stale.sequence = 3;
    auto a = h->sut->append(Batch({Item(), stale}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 2u);
    EXPECT_TRUE((*a)[0].status.ok());
    EXPECT_FALSE((*a)[1].status.ok());
    auto b = h->sut->append(Batch({Item(2)}), Durability::Accepted);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->size(), 1u);
    EXPECT_TRUE((*b)[0].status.ok());
}

TEST_P(JournalContract, PhysicalRangeIsHalfOpen)
{
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    Range range{Range::Axis::Physical, {0, 0}, {100, 0}};
    auto e = h->sut->read(1, range);
    ASSERT_TRUE(e.ok());
    EXPECT_TRUE(e->empty());
    range.start = {100, 0};
    range.end = {101, 0};
    e = h->sut->read(1, range);
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e->size(), 1u);
}

TEST_P(JournalContract, ReleasedIncarnationCannotAppend)
{
    ASSERT_TRUE(h->releaseIncarnation);
    h->releaseIncarnation();
    auto a = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    EXPECT_EQ((*a)[0].status.code(), absl::StatusCode::kFailedPrecondition);
    auto events = h->sut->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_TRUE(events->empty());
}

TEST_P(JournalContract, DurableInvisibleUntilFsyncAndSealDoesNotPassPendingHlc)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "ACCEPTED-only skeleton has no pending WAL fsync";
    ASSERT_TRUE(h->blockFsync);
    ASSERT_TRUE(h->waitPendingHlc);
    ASSERT_TRUE(h->releaseFsync);
    h->blockFsync();
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Durable); });
    struct ReleaseGuard
    {
        std::function<void()> release;
        ~ReleaseGuard() { release(); }
    } guard{h->releaseFsync};
    auto pending = h->waitPendingHlc();
    auto events = h->sut->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_TRUE(events->empty());
    auto frontiers = h->sut->frontier(1);
    ASSERT_TRUE(frontiers.ok());
    ASSERT_FALSE(frontiers->empty());
    for(const auto& frontier: *frontiers) EXPECT_LE(frontier.frontier, pending);
    auto seal = h->sut->keeperFrontier(1);
    ASSERT_TRUE(seal.ok());
    EXPECT_LE(*seal, pending);
    h->releaseFsync();
    guard.release = [] {};
    auto result = append.get();
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->size(), 1u);
    ASSERT_TRUE((*result)[0].status.ok());
    events = h->sut->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
}
TEST_P(JournalContract, RestartResumesAboveReportedFrontier)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "RAM-only Keeper restart creates a new instance";
    ASSERT_TRUE(h->crashRestart);
    auto first = h->sut->append(Batch({Item()}), Durability::Durable);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first->size(), 1u);
    ASSERT_TRUE((*first)[0].status.ok());
    auto frontiers = h->sut->frontier(1);
    ASSERT_TRUE(frontiers.ok());
    ASSERT_EQ(frontiers->size(), 1u);
    auto seal = frontiers->front().frontier;
    h->crashRestart();
    auto next = h->sut->append(Batch({Item(2)}), Durability::Durable);
    ASSERT_TRUE(next.ok());
    ASSERT_EQ(next->size(), 1u);
    ASSERT_TRUE((*next)[0].status.ok());
    EXPECT_GT((*next)[0].hlc, seal);
}
TEST_P(JournalContract, SealedFrontierExceedsEveryAssignmentWhenNothingPending)
{
    auto assigned = h->sut->append(Batch({Item(), Item(2), Item(3)}), Durability::Accepted);
    ASSERT_TRUE(assigned.ok());
    ASSERT_EQ(assigned->size(), 3u);
    auto frontiers = h->sut->frontier(1);
    ASSERT_TRUE(frontiers.ok());
    ASSERT_FALSE(frontiers->empty());
    auto seal = h->sut->keeperFrontier(1);
    ASSERT_TRUE(seal.ok());
    for(const auto& result: *assigned)
    {
        ASSERT_TRUE(result.status.ok());
        EXPECT_GT(*seal, result.hlc);
        for(const auto& frontier: *frontiers) EXPECT_GT(frontier.frontier, result.hlc);
    }
}
TEST_P(JournalContract, IdleRegisteredWriterDoesNotBlockCompleteness)
{
    ASSERT_TRUE(h->registerIdleWriter);
    h->registerIdleWriter(6, 3);
    auto assigned = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(assigned.ok());
    ASSERT_EQ(assigned->size(), 1u);
    ASSERT_TRUE((*assigned)[0].status.ok());
    auto frontiers = h->sut->frontier(1);
    ASSERT_TRUE(frontiers.ok());
    ASSERT_EQ(frontiers->size(), 2u);
    EXPECT_EQ((*frontiers)[0].frontier, (*frontiers)[1].frontier);
    for(const auto& frontier: *frontiers) EXPECT_GT(frontier.frontier, (*assigned)[0].hlc);
    bool idle = false;
    for(const auto& frontier: *frontiers) idle |= frontier.writer_id == 6 && frontier.incarnation == 3;
    EXPECT_TRUE(idle);
}
TEST_P(JournalContract, ItemsAfterAGapAreRejected)
{
    auto results = h->sut->append(Batch({Item(2), Item(1)}), Durability::Accepted);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 2u);
    for(const auto& result: *results) EXPECT_EQ(result.status.code(), absl::StatusCode::kFailedPrecondition);
    auto events = h->sut->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_TRUE(events->empty());
    auto retry = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(retry.ok());
    ASSERT_EQ(retry->size(), 1u);
    EXPECT_EQ(retry->front().status.code(), absl::StatusCode::kOk);
}
TEST_P(JournalContract, RejectsIncompleteEventId)
{
    for(int field = 0; field < 4; ++field)
    {
        auto batch = Batch({Item()});
        if(field == 0)
            batch.story_id = 0;
        if(field == 1)
            batch.items[0].writer_id = 0;
        if(field == 2)
            batch.items[0].incarnation = 0;
        if(field == 3)
            batch.items[0].sequence = 0;
        auto results = h->sut->append(batch, Durability::Accepted);
        if(field == 0)
        {
            EXPECT_EQ(results.status().code(), absl::StatusCode::kInvalidArgument);
        }
        else
        {
            ASSERT_TRUE(results.ok());
            ASSERT_EQ(results->size(), 1u);
            EXPECT_EQ((*results)[0].status.code(), absl::StatusCode::kInvalidArgument);
        }
    }
}
TEST_P(JournalContract, OlderIncarnationIsRejected)
{
    ASSERT_TRUE(h->supersedeIncarnation);
    h->supersedeIncarnation();
    auto results = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].status.code(), absl::StatusCode::kFailedPrecondition);
}
TEST_P(JournalContract, UnassignedKeeperRejectsWriter)
{
    ASSERT_TRUE(h->unassignWriter);
    h->unassignWriter();
    auto results = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE((*results)[0].current_route);
}
TEST_P(JournalContract, AbsurdCausalFloorIsRejected)
{
    auto item = Item();
    item.causal_floor = {100 + h->causal_skew_limit_ns + 1, 0};
    auto results = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 1u);
    EXPECT_EQ((*results)[0].status.code(), absl::StatusCode::kInvalidArgument);
}
TEST_P(JournalContract, AppendResultCarriesAllFourFields)
{
    auto ok = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(ok.ok());
    ASSERT_EQ(ok->size(), 1u);
    EXPECT_EQ(ok->front().status.code(), absl::StatusCode::kOk);
    EXPECT_EQ(ok->front().achieved, Durability::Accepted);
    EXPECT_GT(ok->front().hlc, (Hlc{}));
    EXPECT_EQ(ok->front().id, Identity(Item()));
    auto bad = h->sut->append(Batch({Item(5)}), Durability::Accepted);
    ASSERT_TRUE(bad.ok());
    ASSERT_EQ(bad->size(), 1u);
    EXPECT_EQ(bad->front().status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(bad->front().achieved, Durability::Unspecified);
    EXPECT_EQ(bad->front().id, Identity(Item(5)));
    EXPECT_EQ(bad->front().hlc, (Hlc{}));
    EXPECT_NE(bad->front().status.message().find("expected sequence 2"), std::string::npos);
}

TEST_P(JournalContract, FsyncFailureFailsTheGroup)
{
    if(!h->failFsync)
        GTEST_SKIP() << "RAM Journal has no WAL fsync";
    h->blockFsync();
    h->failFsync(true);
    auto append = std::async(std::launch::async,
                             [&] { return h->sut->append(Batch({Item(), Item(2), Item(3)}), Durability::Durable); });
    (void)h->waitPendingHlc();
    h->releaseFsync();
    ASSERT_EQ(append.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto results = append.get();
    ASSERT_TRUE(results.ok());
    ASSERT_EQ(results->size(), 3u);
    for(const auto& result: *results)
    {
        EXPECT_EQ(result.status.code(), absl::StatusCode::kUnavailable);
        EXPECT_EQ(result.achieved, Durability::Unspecified);
    }
    EXPECT_EQ(eventCount(), 0u);
    h->failFsync(false);
    auto failed = h->sut->append(Batch({Item(4)}), Durability::Durable);
    ASSERT_TRUE(failed.ok());
    EXPECT_EQ(failed->front().status.code(), absl::StatusCode::kUnavailable);
    auto accepted = h->sut->append(Batch({Item(4)}), Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    EXPECT_EQ(accepted->front().status.code(), absl::StatusCode::kOk);
}

TEST_P(JournalContract, WriterCreatedBeforeFirstAssignmentIsScanned)
{
    if(!h->onAssignment || !h->snapshot)
        GTEST_SKIP() << "assignment scheduling control unavailable";
    h->registerIdleWriter(6, 1);
    std::promise<Hlc> assigned;
    std::promise<void> resume;
    auto resumed = resume.get_future().share();
    h->onAssignment(
            [&](Hlc hlc)
            {
                assigned.set_value(hlc);
                if(resumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                    throw std::runtime_error("assignment timeout");
            });
    auto item = Item();
    item.writer_id = 6;
    item.incarnation = 1;
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({item}), Durability::Accepted); });
    auto assigned_future = assigned.get_future();
    ASSERT_EQ(assigned_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto hlc = assigned_future.get();
    auto scan = std::async(std::launch::async, [&] { return h->snapshot(); });
    EXPECT_EQ(scan.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    resume.set_value();
    auto result = append.get();
    h->onAssignment({});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->front().status.code(), absl::StatusCode::kOk);
    auto snapshot = scan.get();
    ASSERT_TRUE(snapshot.ok());
    ASSERT_EQ(snapshot->second.size(), 1u);
    EXPECT_EQ(snapshot->second.front().id, Identity(item));
    EXPECT_GT(snapshot->first, hlc);
}

TEST_P(JournalContract, InFlightAssignmentCannotEscapeSealSnapshot)
{
    if(!h->onAssignment || !h->snapshot)
        GTEST_SKIP() << "assignment scheduling control unavailable";
    auto first = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first->front().status.code(), absl::StatusCode::kOk);
    std::promise<void> assigned, resume;
    auto resumed = resume.get_future().share();
    h->onAssignment(
            [&](Hlc)
            {
                assigned.set_value();
                if(resumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                    throw std::runtime_error("assignment timeout");
            });
    auto append =
            std::async(std::launch::async, [&] { return h->sut->append(Batch({Item(2)}), Durability::Accepted); });
    auto assigned_future = assigned.get_future();
    ASSERT_EQ(assigned_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto scan = std::async(std::launch::async, [&] { return h->snapshot(); });
    EXPECT_EQ(scan.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    resume.set_value();
    auto result = append.get();
    h->onAssignment({});
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result->front().status.code(), absl::StatusCode::kOk);
    auto snapshot = scan.get();
    ASSERT_TRUE(snapshot.ok());
    ASSERT_EQ(snapshot->second.size(), 2u);
    EXPECT_EQ(snapshot->second.back().id, Identity(Item(2)));
    EXPECT_GT(snapshot->first, result->front().hlc);
}

TEST_P(JournalContract, PendingFsyncRegisteredWithAssignment)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "RAM Journal has no pending fsync";
    ASSERT_TRUE(h->onAssignment);
    ASSERT_TRUE(h->snapshot);
    std::promise<void> assigned, resume;
    auto resumed = resume.get_future().share();
    h->onAssignment(
            [&](Hlc)
            {
                assigned.set_value();
                if(resumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                    throw std::runtime_error("assignment timeout");
            });
    h->blockFsync();
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Durable); });
    auto assigned_future = assigned.get_future();
    ASSERT_EQ(assigned_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto scan = std::async(std::launch::async, [&] { return h->snapshot(); });
    EXPECT_EQ(scan.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    resume.set_value();
    const auto pending = h->waitPendingHlc();
    auto snapshot = scan.get();
    h->releaseFsync();
    auto result = append.get();
    h->onAssignment({});
    ASSERT_TRUE(snapshot.ok());
    EXPECT_TRUE(snapshot->second.empty());
    EXPECT_LE(snapshot->first, pending);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->front().status.code(), absl::StatusCode::kOk);
}

TEST_P(JournalContract, FsyncCompletesBetweenScanAndCapDoesNotLoseEvent)
{
    if(!h->onWriterScanned)
        GTEST_SKIP() << "RAM Journal has no pending fsync";
    h->blockFsync();
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Durable); });
    const auto pending = h->waitPendingHlc();
    std::promise<void> scanned, resume;
    auto resumed = resume.get_future().share();
    h->onWriterScanned(
            [&]
            {
                scanned.set_value();
                if(resumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                    throw std::runtime_error("scan timeout");
            });
    auto scan = std::async(std::launch::async, [&] { return h->snapshot(); });
    auto scanned_future = scanned.get_future();
    EXPECT_EQ(scanned_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    h->releaseFsync();
    auto result = append.get();
    resume.set_value();
    auto snapshot = scan.get();
    h->onWriterScanned({});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->front().status.code(), absl::StatusCode::kOk);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_TRUE(snapshot->second.empty());
    EXPECT_LE(snapshot->first, pending);
    auto next = h->snapshot();
    ASSERT_TRUE(next.ok());
    ASSERT_EQ(next->second.size(), 1u);
    EXPECT_GT(next->first, pending);
}

TEST_P(JournalContract, FsyncCompletionFlipsVisibilityUnderWriterLock)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "RAM Journal has no pending fsync";
    ASSERT_TRUE(h->snapshot);
    for(uint64_t sequence = 1; sequence <= 16; ++sequence)
    {
        h->blockFsync();
        auto append = std::async(std::launch::async,
                                 [&] { return h->sut->append(Batch({Item(sequence)}), Durability::Durable); });
        const auto pending = h->waitPendingHlc();
        std::promise<void> started;
        auto scan = std::async(std::launch::async,
                               [&]
                               {
                                   started.set_value();
                                   for(int i = 0; i < 32; ++i)
                                   {
                                       auto snapshot = h->snapshot();
                                       EXPECT_TRUE(snapshot.ok());
                                       if(!snapshot.ok())
                                           return;
                                       bool visible = false;
                                       for(const auto& event: snapshot->second)
                                           visible |= event.id == Identity(Item(sequence));
                                       EXPECT_TRUE(visible || snapshot->first <= pending);
                                       std::this_thread::yield();
                                   }
                               });
        started.get_future().wait();
        h->releaseFsync();
        auto result = append.get();
        scan.get();
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result->front().status.code(), absl::StatusCode::kOk);
        EXPECT_EQ(eventCount(), sequence);
    }
}

TEST_P(JournalContract, EvictionRequiresWatermarkAndReceipt)
{
    if(!h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no archive retention";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->releaseTail();
    h->deliverChunk(*chunk, "g1", 5);
    h->reportArchive({1, {100, 0}, "g1", 5, {}, false});
    EXPECT_EQ(eventCount(), 1u);
    h->reportArchive({1, chunk->end, "g1", 5, {}, false});
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, DroppedStoryReportFreesRetainedChunks)
{
    if(!h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no archive retention";
    auto first = archiveChunk();
    ASSERT_TRUE(first.ok());
    auto second = archiveChunk(2);
    ASSERT_TRUE(second.ok());
    h->reportArchive({1, {}, "g1", 0, {}, true});
    EXPECT_EQ(eventCount(), 0u);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    EXPECT_TRUE(chunks->empty());
    EXPECT_EQ(h->evictionFloor(), second->end);
}
TEST_P(JournalContract, ReceiptFromAnotherGrapherInstanceIsNotSettled)
{
    if(!h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no archive retention";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->deliverChunk(*chunk, "g1", 5);
    h->releaseTail();
    h->reportArchive({1, chunk->end, "g2", 50, {}, false});
    EXPECT_EQ(eventCount(), 1u);
    h->reportArchive({1, chunk->end, "g1", 5, {}, false});
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, CoveringWatermarkAloneDoesNotSettleReceipt)
{
    if(!h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no archive retention";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->deliverChunk(*chunk, "g1", 5);
    h->releaseTail();
    h->reportArchive({1, chunk->end, "g1", 4, {}, false});
    EXPECT_EQ(eventCount(), 1u);
    h->reportArchive({1, chunk->end, "g1", 5, {5}, false});
    EXPECT_EQ(eventCount(), 1u);
    h->reportArchive({1, chunk->end, "g1", 5, {}, false});
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, WalTruncatesOnlyAfterReceiptSettlement)
{
    if(!h->walSegments || !h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no WAL segments";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->setPhysical(1'100'000'000);
    auto second = h->sut->append(Batch({Item(2)}), Durability::Durable);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second->front().status.code(), absl::StatusCode::kOk);
    auto before = h->walSegments();
    ASSERT_GT(before.size(), 1u);
    h->deliverChunk(*chunk, "g1", 5);
    h->reportArchive({1, chunk->end, "g2", 100, {}, false});
    EXPECT_EQ(h->walSegments(), before);
    h->reportArchive({1, chunk->end, "g1", 5, {5}, false});
    EXPECT_EQ(h->walSegments(), before);
    h->reportArchive({1, {}, "g1", 5, {}, false});
    auto after = h->walSegments();
    size_t removed = 0;
    for(const auto& segment: before) removed += !after.contains(segment);
    EXPECT_GT(removed, 0u);
    h->crashRestart();
    EXPECT_EQ(eventCount(), 1u);
    EXPECT_EQ(h->evictionFloor(), chunk->end);
    auto retry = h->sut->append(Batch({Item()}), Durability::Durable);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->front().status.code(), absl::StatusCode::kOk);
    EXPECT_EQ(retry->front().achieved, Durability::Durable);
    h->setPhysical(5'100'000'000);
    auto next = h->sut->append(Batch({Item(3)}), Durability::Durable);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next->front().status.code(), absl::StatusCode::kOk);
    h->setPhysical(6'000'000'000);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    ASSERT_FALSE(chunks->empty());
    EXPECT_EQ(chunks->front().start, chunk->end);
}
TEST_P(JournalContract, ReassignedWriterObservesOldFrontier)
{
    GTEST_SKIP() << "M8 dynamic reassignment is not implemented";
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(JournalContract);
} // namespace chronolog::contract
