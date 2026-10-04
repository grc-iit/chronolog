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
struct CheckpointStatus
{
    bool known{}, released{};
    uint64_t next_sequence{};
    Hlc last_hlc;
    AcquisitionTerminationCause termination_cause{};
    std::optional<AppendResult> recorded;
};
struct JournalHarness
{
    std::function<absl::StatusOr<CheckpointStatus>(EventId)> writerStatus;
    std::function<void()> evictEvents;
    std::unique_ptr<Journal> sut;
    size_t payload_limit{1024 * 1024};
    int64_t causal_skew_limit_ns{1000};
    std::function<void()> supersedeIncarnation;
    std::function<void()> unassignWriter;
    bool supports_durable{};
    bool rejection_reasons{};
    std::function<size_t()> drainAdmissionEvidence;
    std::function<void(AcquisitionTerminationCause)> terminateIncarnation;
    size_t dedupe_window{};
    std::function<void(bool)> applySupersession;
    // Pause after slot validation, before acquiring the writer lock.
    std::function<void(std::function<void()>)> onSlotValidated;
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
    std::function<void(Hlc, int64_t)> enableDynamic;
    std::function<void(Hlc, int64_t)> extendCeiling;
    std::function<bool()> ceilingWaiting;
    std::function<void(RouteState, bool, uint64_t)> applyRoute;
    std::function<int64_t()> acceptanceClock;
    std::function<bool()> retiredDrained;
    // Observe the recovered eviction floor.
    std::function<Hlc()> evictionFloor;
    // Apply the Visor tombstone of story 1, as the route stream or the Catalog reconciliation delivers it.
    std::function<void()> tombstone;
    std::function<void(bool)> forceCapacity;
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

TEST_P(JournalContract, AppendRejectionReasons)
{
    ASSERT_TRUE(h->rejection_reasons);
    const auto check = [&](AppendBatch batch, AppendRejection reason)
    {
        auto results = h->sut->append(batch, Durability::Accepted);
        ASSERT_TRUE(results.ok()) << results.status();
        ASSERT_EQ(results->size(), batch.items.size());
        EXPECT_EQ(results->front().status.code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(results->front().rejection, reason);
    };
    auto unknown = Item();
    unknown.writer_id = 99;
    check(Batch({unknown}), AppendRejection::NotRegistered);
    unknown = Item();
    unknown.incarnation = 4;
    check(Batch({unknown}), AppendRejection::NotRegistered);
    check(Batch({Item()}, 6), AppendRejection::StaleEpoch);
    auto gap = h->sut->append(Batch({Item(2), Item()}), Durability::Accepted);
    ASSERT_TRUE(gap.ok());
    ASSERT_EQ(gap->size(), 2u);
    EXPECT_EQ(gap->at(0).rejection, AppendRejection::SequenceGap);
    EXPECT_EQ(gap->at(1).rejection, AppendRejection::EarlierItemFailed);
    for(const auto& result: *gap) EXPECT_EQ(result.status.code(), absl::StatusCode::kFailedPrecondition);
    h->releaseIncarnation();
    check(Batch({Item()}), AppendRejection::FencedReleased);
    h->supersedeIncarnation();
    check(Batch({Item()}), AppendRejection::FencedSuperseded);
    h->unassignWriter();
    check(Batch({Item()}), AppendRejection::UnassignedKeeper);
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "grapher:1", ""};
    h->applyRoute(state, false, 10);
    check(Batch({Item()}, 8), AppendRejection::KeeperNotInRoute);
    h->tombstone();
    check(Batch({Item()}, 8), AppendRejection::StoryTombstoned);
}

TEST_P(JournalContract, OnlyNewAdmissionProducesLeaseLiveness)
{
    ASSERT_TRUE(h->drainAdmissionEvidence);
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
    const auto durability = h->supports_durable ? Durability::Durable : Durability::Accepted;
    auto first = h->sut->append(Batch({Item(), Item(2)}), Durability::Accepted);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first->at(0).status.ok());
    EXPECT_EQ(h->drainAdmissionEvidence(), 1u);
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
    auto retry = h->sut->append(Batch({Item(), Item(2)}), durability);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
    auto gap = h->sut->append(Batch({Item(4)}), durability);
    ASSERT_TRUE(gap.ok());
    EXPECT_EQ(gap->at(0).rejection, AppendRejection::SequenceGap);
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
    auto rejected = Item(3);
    rejected.physical.physical_ns = 100 + PhysicalPolicy{}.skew_limit_ns + 2;
    auto out = h->sut->append(Batch({rejected}), durability);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out->at(0).status.code(), absl::StatusCode::kOutOfRange);
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
    auto next = h->sut->append(Batch({Item(4)}), durability);
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(next->at(0).status.ok());
    h->crashRestart();
    EXPECT_EQ(h->drainAdmissionEvidence(), 0u);
}

TEST_P(JournalContract, AppendRejectionReasonsTerminationCauses)
{
    for(auto cause: {AcquisitionTerminationCause::Expired, AcquisitionTerminationCause::OwnerRemoved})
    {
        h = GetParam()();
        ASSERT_TRUE(h->terminateIncarnation);
        const auto reason = cause == AcquisitionTerminationCause::Expired ? AppendRejection::FencedExpired
                                                                          : AppendRejection::FencedOwnerRemoved;
        for(size_t i = 1; i <= h->dedupe_window + 1; ++i)
        {
            auto admitted = h->sut->append(Batch({Item(i)}), Durability::Accepted);
            ASSERT_TRUE(admitted.ok());
            ASSERT_TRUE(admitted->at(0).status.ok());
        }
        h->terminateIncarnation(cause);
        auto check = [&]
        {
            auto result = h->sut->append(Batch({Item(), Item(h->dedupe_window + 2)}), Durability::Accepted);
            ASSERT_TRUE(result.ok());
            for(const auto& item: *result)
            {
                EXPECT_EQ(item.status.code(), absl::StatusCode::kFailedPrecondition);
                EXPECT_EQ(item.rejection, reason);
            }
        };
        check();
        h->releaseIncarnation();
        h->supersedeIncarnation();
        check();
    }
}

TEST_P(JournalContract, AppendRejectionReasonsDedupe)
{
    ASSERT_TRUE(h->rejection_reasons);
    ASSERT_GT(h->dedupe_window, 0u);
    ASSERT_LE(h->dedupe_window, 65536u);
    const auto durability = h->supports_durable ? Durability::Durable : Durability::Accepted;
    auto item = Item();
    item.physical.physical_ns = 100 + PhysicalPolicy{}.skew_limit_ns + 2;
    auto rejected = h->sut->append(Batch({item}), durability);
    ASSERT_TRUE(rejected.ok());
    ASSERT_EQ(rejected->front().status.code(), absl::StatusCode::kOutOfRange);
    EXPECT_EQ(rejected->front().rejection, AppendRejection::Unspecified);
    auto duplicate = h->sut->append(Batch({Item()}), durability);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_EQ(duplicate->front().status, rejected->front().status);
    EXPECT_EQ(duplicate->front().rejection, rejected->front().rejection);
    auto accepted = h->sut->append(Batch({Item(2)}), durability);
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(accepted->front().status.ok());
    if(h->supports_durable)
        h->crashRestart();
    for(auto sequence: {1u, 2u})
    {
        auto retry = h->sut->append(Batch({Item(sequence)}), durability);
        ASSERT_TRUE(retry.ok());
        EXPECT_EQ(retry->front().status.code(), sequence == 1 ? absl::StatusCode::kOutOfRange : absl::StatusCode::kOk);
        EXPECT_EQ(retry->front().rejection, AppendRejection::Unspecified);
    }
    std::vector<AppendItem> items;
    for(size_t i = 3; i <= h->dedupe_window + 2; ++i) items.push_back(Item(i));
    auto filled = h->sut->append(Batch(std::move(items)), Durability::Accepted);
    ASSERT_TRUE(filled.ok());
    for(const auto& result: *filled) ASSERT_TRUE(result.status.ok()) << result.status;
    auto outside = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(outside.ok());
    EXPECT_EQ(outside->front().status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(outside->front().rejection, AppendRejection::DedupeWindow);
}

TEST_P(JournalContract, SupersessionReleaseOrdersOldAdmissionBeforeNewMarker)
{
    ASSERT_TRUE(h->rejection_reasons);
    ASSERT_TRUE(h->onSlotValidated);
    ASSERT_TRUE(h->applySupersession);
    using namespace std::chrono_literals;
    for(bool snapshot: {false, true})
    {
        h = GetParam()();
        std::promise<void> validated, resume;
        auto ready = validated.get_future();
        auto resumed = resume.get_future().share();
        h->onSlotValidated(
                [&]
                {
                    validated.set_value();
                    EXPECT_EQ(resumed.wait_for(5s), std::future_status::ready);
                });
        auto old =
                std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Accepted); });
        const auto reached = ready.wait_for(5s);
        if(reached != std::future_status::ready)
        {
            resume.set_value();
            FAIL() << "old append did not validate its slot";
        }
        h->onSlotValidated({});
        h->applySupersession(snapshot);
        auto marker = Item();
        marker.incarnation = 4;
        auto marked = h->sut->append(Batch({marker}), Durability::Accepted);
        resume.set_value();
        auto refused = old.get();
        ASSERT_TRUE(marked.ok());
        ASSERT_TRUE(marked->front().status.ok());
        ASSERT_TRUE(refused.ok());
        EXPECT_EQ(refused->front().status.code(), absl::StatusCode::kFailedPrecondition);
        EXPECT_EQ(refused->front().rejection, AppendRejection::FencedReleased);
        EXPECT_EQ(eventCount(), 1u);

        h = GetParam()();
        ASSERT_TRUE(h->onAssignment);
        std::promise<void> assigned, finish_assignment;
        auto assigning = assigned.get_future();
        auto finishing = finish_assignment.get_future().share();
        h->onAssignment(
                [&](Hlc)
                {
                    assigned.set_value();
                    EXPECT_EQ(finishing.wait_for(5s), std::future_status::ready);
                });
        auto admitted_call =
                std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Accepted); });
        if(assigning.wait_for(5s) != std::future_status::ready)
        {
            finish_assignment.set_value();
            FAIL() << "old append did not reach assignment";
        }
        h->onAssignment({});
        auto takeover = std::async(std::launch::async,
                                   [&]
                                   {
                                       h->applySupersession(snapshot);
                                       return h->sut->append(Batch({marker}), Durability::Accepted);
                                   });
        finish_assignment.set_value();
        auto admitted = admitted_call.get();
        marked = takeover.get();
        ASSERT_TRUE(admitted.ok());
        ASSERT_TRUE(admitted->front().status.ok());
        ASSERT_TRUE(marked.ok());
        ASSERT_TRUE(marked->front().status.ok());
        EXPECT_LT(admitted->front().hlc, marked->front().hlc);
    }
}

TEST_P(JournalContract, AppendRejectionDefaultsToUnspecified)
{
    EXPECT_EQ(AppendResult{}.rejection, AppendRejection::Unspecified);
    auto items = std::vector<AppendItem>{Item(), Item(2)};
    for(auto durability: {Durability::Accepted, Durability::Durable})
    {
        if(durability == Durability::Durable && !h->supports_durable)
            continue;
        auto result = h->sut->append(Batch(items), durability);
        ASSERT_TRUE(result.ok()) << result.status();
        ASSERT_EQ(result->size(), items.size());
        for(const auto& item: *result)
        {
            ASSERT_TRUE(item.status.ok()) << item.status;
            EXPECT_EQ(item.rejection, AppendRejection::Unspecified);
        }
    }
    auto retry = h->sut->append(Batch(items), Durability::Accepted);
    ASSERT_TRUE(retry.ok()) << retry.status();
    ASSERT_EQ(retry->size(), items.size());
    for(const auto& item: *retry)
    {
        ASSERT_TRUE(item.status.ok()) << item.status;
        EXPECT_EQ(item.rejection, AppendRejection::Unspecified);
    }
}

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
    // I5.4: the retry's achieved durability is equal or higher, never lower.
    EXPECT_GE(static_cast<int>((*b)[0].achieved), static_cast<int>((*a)[0].achieved));
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
    EXPECT_EQ((*a)[0].achieved, Durability::Unspecified);
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
    EXPECT_EQ((*a)[0].achieved, Durability::Unspecified);
    i = Item();
    i.envelope.trace_id = "short";
    auto b = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(b->size(), 1u);
    EXPECT_EQ((*b)[0].status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*b)[0].achieved, Durability::Unspecified);
    i = Item();
    i.envelope.span_id = "short";
    auto c = h->sut->append(Batch({i}), Durability::Accepted);
    ASSERT_TRUE(c.ok());
    ASSERT_EQ(c->size(), 1u);
    EXPECT_EQ((*c)[0].status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ((*c)[0].achieved, Durability::Unspecified);
}

namespace
{
Link LinkTo(uint64_t sequence, std::string type = "caused-by")
{
    return {std::move(type), {1, 9, 8, sequence}, std::nullopt};
}
absl::StatusCode ItemCode(JournalHarness& h, const AppendItem& item)
{
    auto results = h.sut->append(Batch({item}), Durability::Accepted);
    EXPECT_TRUE(results.ok());
    return results.ok() && results->size() == 1 ? (*results)[0].status.code() : absl::StatusCode::kUnknown;
}
} // namespace

TEST_P(JournalContract, EnvelopeKindActorLinksRoundTrip)
{
    auto i = Item();
    i.envelope.kind = "decision";
    i.envelope.actor = "agent-7";
    i.envelope.links = {LinkTo(5), {"replies-to", {1, 4, 5, 6}, Hlc{77, 3}}};
    const auto durability = h->supports_durable ? Durability::Durable : Durability::Accepted;
    auto a = h->sut->append(Batch({i}), durability);
    ASSERT_TRUE(a.ok());
    ASSERT_EQ(a->size(), 1u);
    ASSERT_TRUE((*a)[0].status.ok());
    auto check = [&]
    {
        auto e = h->sut->read(1, All());
        ASSERT_TRUE(e.ok());
        ASSERT_EQ(e->size(), 1u);
        EXPECT_EQ((*e)[0].envelope.kind, i.envelope.kind);
        EXPECT_EQ((*e)[0].envelope.actor, i.envelope.actor);
        EXPECT_EQ((*e)[0].envelope.links, i.envelope.links);
    };
    check();
    if(h->supports_durable && h->crashRestart)
    {
        h->crashRestart();
        check();
    }
}

TEST_P(JournalContract, EnvelopeFieldLimitsRejectPerItem)
{
    auto at_limit = Item(1);
    at_limit.envelope.kind.assign(64, 'k');
    at_limit.envelope.actor.assign(256, 'a');
    at_limit.envelope.links.assign(16, LinkTo(5, std::string(64, 't')));
    auto over = [&](auto mutate)
    {
        auto item = Item(1);
        mutate(item.envelope);
        return item;
    };
    EXPECT_EQ(ItemCode(*h, over([](Envelope& e) { e.kind.assign(65, 'k'); })), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(ItemCode(*h, over([](Envelope& e) { e.actor.assign(257, 'a'); })), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(ItemCode(*h, over([](Envelope& e) { e.links.assign(17, LinkTo(5)); })),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(ItemCode(*h, over([](Envelope& e) { e.links = {LinkTo(5, std::string(65, 't'))}; })),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(ItemCode(*h, over([](Envelope& e) { e.links = {LinkTo(5, "")}; })), absl::StatusCode::kInvalidArgument);
    for(int field = 0; field < 4; ++field)
        EXPECT_EQ(ItemCode(*h,
                           over(
                                   [&](Envelope& e)
                                   {
                                       Link link = LinkTo(5);
                                       (field == 0   ? link.target.story_id
                                        : field == 1 ? link.target.writer_id
                                        : field == 2 ? link.target.incarnation
                                                     : link.target.sequence) = 0;
                                       e.links = {link};
                                   })),
                  absl::StatusCode::kInvalidArgument);
    // Rejections consume nothing: the same sequence admits a valid item, and the limits themselves are accepted.
    auto accepted = h->sut->append(Batch({at_limit}), Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    ASSERT_EQ(accepted->size(), 1u);
    EXPECT_TRUE((*accepted)[0].status.ok());
    auto e = h->sut->read(1, All());
    ASSERT_TRUE(e.ok());
    ASSERT_EQ(e->size(), 1u);
    EXPECT_EQ((*e)[0].envelope.kind, at_limit.envelope.kind);
}

TEST_P(JournalContract, ReservedKindAndLinkTypeAreRejected)
{
    auto kind = Item();
    kind.envelope.kind = "chronolog.marker";
    EXPECT_EQ(ItemCode(*h, kind), absl::StatusCode::kInvalidArgument);
    auto type = Item();
    type.envelope.links = {LinkTo(5, "chronolog.supersedes")};
    EXPECT_EQ(ItemCode(*h, type), absl::StatusCode::kInvalidArgument);
    // Only the prefix is reserved, and attribute keys never are.
    auto ok = Item();
    ok.envelope.kind = "chronolog";
    ok.envelope.links = {LinkTo(5, "chronologx.caused-by")};
    ok.envelope.attributes["chronolog.operation.id"] = "x";
    EXPECT_EQ(ItemCode(*h, ok), absl::StatusCode::kOk);
    EXPECT_EQ(eventCount(), 1u);
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
TEST_P(JournalContract, TombstonedStoryRefusesAppend)
{
    auto before = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(before.ok());
    ASSERT_TRUE(before->front().status.ok());
    h->tombstone();
    auto after = h->sut->append(Batch({Item(2)}), Durability::Accepted);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->front().status.code(), absl::StatusCode::kFailedPrecondition);
    // An idempotent retry of an acknowledged append is refused too, and FetchHot reads fail whole-request.
    auto retry = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->front().status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(h->snapshot().status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, TombstoneFreesRetainedChunks)
{
    if(!h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no archive retention";
    auto first = archiveChunk();
    ASSERT_TRUE(first.ok());
    auto second = archiveChunk(2);
    ASSERT_TRUE(second.ok());
    h->tombstone();
    EXPECT_EQ(eventCount(), 0u);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    EXPECT_TRUE(chunks->empty());
    EXPECT_EQ(h->evictionFloor(), second->end);
}
TEST_P(JournalContract, WalTruncatesDroppedStoryRecords)
{
    if(!h->walSegments || !h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no WAL segments";
    // No chunk is delivered, so only the story-drop record can settle these records.
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->setPhysical(1'100'000'000);
    auto second = h->sut->append(Batch({Item(2)}), Durability::Durable);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second->front().status.code(), absl::StatusCode::kOk);
    auto before = h->walSegments();
    ASSERT_GT(before.size(), 1u);
    h->tombstone();
    auto after = h->walSegments();
    size_t removed = 0;
    for(const auto& segment: before) removed += !after.contains(segment);
    EXPECT_GT(removed, 0u);
    h->crashRestart();
    EXPECT_EQ(eventCount(), 0u);
}
// I13.11: a Keeper that freed on a dropped=true report alone writes no D record, so the story's WAL segments stay
// pinned, across a restart too, until the tombstone itself arrives.
TEST_P(JournalContract, ReportOnlyDropPinsWalSegmentsAcrossRestart)
{
    if(!h->walSegments || !h->sealArchive)
        GTEST_SKIP() << "RAM Journal has no WAL segments";
    // One sealed, undelivered chunk and one unsealed event: nothing but a drop can settle their records.
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->setPhysical(1'100'000'000);
    auto second = h->sut->append(Batch({Item(2)}), Durability::Durable);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second->front().status.code(), absl::StatusCode::kOk);
    const auto held = eventCount();
    ASSERT_EQ(held, 2u);
    const auto before = h->walSegments();
    ASSERT_GT(before.size(), 1u);
    h->reportArchive({1, {}, "g1", 0, {}, true});
    EXPECT_EQ(eventCount(), 0u);
    const auto pinned = h->walSegments();
    for(const auto& segment: before) EXPECT_TRUE(pinned.contains(segment)) << segment << " removed on a report alone";
    // No D record and no settlement was written: the restart replays every record the report freed from RAM.
    h->crashRestart();
    EXPECT_EQ(eventCount(), held);
    // The tombstone settles the story: its D record lets the segments go and a second restart keeps it dropped.
    const auto replayed = h->walSegments();
    h->tombstone();
    const auto after = h->walSegments();
    size_t removed = 0;
    for(const auto& segment: replayed) removed += !after.contains(segment);
    EXPECT_GT(removed, 0u);
    h->crashRestart();
    EXPECT_EQ(eventCount(), 0u);
    auto refused = h->sut->append(Batch({Item(9)}), Durability::Accepted);
    ASSERT_TRUE(refused.ok());
    EXPECT_EQ(refused->front().status.code(), absl::StatusCode::kFailedPrecondition);
}
TEST_P(JournalContract, DroppedStorySurvivesRestart)
{
    if(!h->walSegments || !h->sealArchive)
        GTEST_SKIP() << "RAM Journal keeps nothing across a restart";
    // Tombstone first, then report first: the report alone frees but writes no D record, and the tombstone that
    // follows it does. Either order leaves the story refused and empty after a crash.
    for(const bool report_first: {false, true})
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_TRUE(archiveChunk().ok());
        if(report_first)
        {
            h->reportArchive({1, {}, "g1", 0, {}, true});
            EXPECT_EQ(eventCount(), 0u);
            auto during = h->sut->append(Batch({Item(8)}), Durability::Accepted);
            ASSERT_TRUE(during.ok());
            EXPECT_EQ(during->front().status.code(), absl::StatusCode::kFailedPrecondition);
        }
        h->tombstone();
        h->crashRestart();
        auto after = h->sut->append(Batch({Item(9)}), Durability::Accepted);
        ASSERT_TRUE(after.ok());
        EXPECT_EQ(after->front().status.code(), absl::StatusCode::kFailedPrecondition)
                << "report_first=" << report_first;
        EXPECT_EQ(eventCount(), 0u);
    }
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
    h->enableDynamic({10000, 0}, 10'000'000'000);
    RouteState state;
    state.route = {8, {{"self", "self:1"}}, "grapher:1", "player:1"};
    state.ordering_cut = {500, 0};
    h->applyRoute(state, true, 10);
    auto result = h->sut->append(Batch({Item()}, 8), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    EXPECT_GT(result->front().hlc, state.ordering_cut);
}

TEST_P(JournalContract, BackdatedReadingIsOutOfRange)
{
    auto item = Item();
    item.physical.physical_ns = 100 - PhysicalPolicy{}.acceptance_window_ns - 2;
    auto result = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsOutOfRange(result->front().status));
    EXPECT_EQ(result->front().achieved, Durability::Unspecified);
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, FutureReadingIsOutOfRange)
{
    auto item = Item();
    item.physical.physical_ns = 100 + PhysicalPolicy{}.skew_limit_ns + 2;
    auto result = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsOutOfRange(result->front().status));
    EXPECT_EQ(eventCount(), 0u);
}
TEST_P(JournalContract, UnrepresentableIntervalIsInvalidArgument)
{
    for(auto physical: {INT64_MIN, INT64_MAX})
    {
        auto item = Item();
        item.physical.physical_ns = physical;
        auto result = h->sut->append(Batch({item}), Durability::Accepted);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(absl::IsInvalidArgument(result->front().status));
    }
    auto valid = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(valid.ok());
    EXPECT_TRUE(valid->front().status.ok());
}
TEST_P(JournalContract, OutOfRangeConsumesSequenceAndRetriesIdempotently)
{
    auto item = Item();
    item.physical.physical_ns = 100 + PhysicalPolicy{}.skew_limit_ns + 2;
    auto first = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(absl::IsOutOfRange(first->front().status));
    if(h->supports_durable)
        h->crashRestart();
    auto retry = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->front().status, first->front().status);
    EXPECT_EQ(retry->front().achieved, Durability::Unspecified);
    auto next = h->sut->append(Batch({Item(2)}), Durability::Accepted);
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE(next->front().status.ok());
    EXPECT_EQ(eventCount(), 1u);
}
TEST_P(JournalContract, HlcLeadOverAcceptanceClockIsBounded)
{
    for(uint64_t sequence = 1; sequence <= 32; ++sequence)
    {
        auto item = Item(sequence);
        item.causal_floor = {100 + h->causal_skew_limit_ns, UINT32_MAX};
        auto result = h->sut->append(Batch({item}), Durability::Accepted);
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(result->front().status.ok());
        EXPECT_GE(result->front().hlc.physical_ns, 100);
        EXPECT_LE(result->front().hlc.physical_ns - 100, PhysicalPolicy{}.hlc_lead_ns);
    }
}
TEST_P(JournalContract, PhysicalFrontierBoundsLaterAcceptance)
{
    h->setPhysical(30'000'000'000);
    auto frontier = h->sut->physicalFrontier(1);
    ASSERT_TRUE(frontier.ok());
    auto item = Item();
    item.physical.physical_ns = *frontier;
    auto result = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsOutOfRange(result->front().status));
    item = Item(2);
    item.physical.physical_ns = *frontier + 1;
    result = h->sut->append(Batch({item}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->front().status.ok());
}
TEST_P(JournalContract, PhysicalFrontierSurvivesRestart)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "requires WAL";
    h->setPhysical(30'000'000'000);
    auto before = h->sut->physicalFrontier(1);
    ASSERT_TRUE(before.ok());
    h->crashRestart();
    auto after = h->sut->physicalFrontier(1);
    ASSERT_TRUE(after.ok());
    EXPECT_GE(*after, *before);
    auto result = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsOutOfRange(result->front().status));
}
TEST_P(JournalContract, PhysicalCheckOrderedBeforeInsert)
{
    if(!h->onAssignment)
        GTEST_SKIP() << "requires assignment hook";
    std::promise<void> assigned, resume;
    auto resumed = resume.get_future().share();
    h->onAssignment(
            [&](Hlc)
            {
                assigned.set_value();
                if(resumed.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                    throw std::runtime_error("assignment timeout");
            });
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Accepted); });
    auto ready = assigned.get_future();
    EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    h->setPhysical(30'000'000'000);
    auto report = std::async(std::launch::async, [&] { return h->sut->physicalFrontier(1); });
    EXPECT_EQ(report.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    resume.set_value();
    auto result = append.get();
    h->onAssignment({});
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->front().status.ok());
    auto frontier = report.get();
    ASSERT_TRUE(frontier.ok());
    EXPECT_GE(*frontier, 15'000'000'000);
    EXPECT_EQ(eventCount(), 1u);
}
TEST_P(JournalContract, DurabilityUpgradeDoesNotLowerPhysicalFrontier)
{
    if(!h->blockFsync)
        GTEST_SKIP() << "requires WAL";
    auto accepted = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    h->setPhysical(30'000'000'000);
    auto before = h->sut->physicalFrontier(1);
    ASSERT_TRUE(before.ok());
    h->blockFsync();
    auto upgrade = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Durable); });
    (void)h->waitPendingHlc();
    auto report = std::async(std::launch::async, [&] { return h->sut->physicalFrontier(1); });
    h->releaseFsync();
    auto result = upgrade.get();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok());
    auto after = report.get();
    ASSERT_TRUE(after.ok());
    EXPECT_GE(*after, *before);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(JournalContract);
} // namespace chronolog::contract

namespace chronolog::contract
{
TEST_P(JournalContract, NoAssignmentAtOrAboveCeiling)
{
    h->enableDynamic({100, 0}, 10'000'000'000);
    auto result = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsUnavailable(result->front().status));
    EXPECT_EQ(eventCount(), 0);
    h->extendCeiling({10000, 0}, 10'000'000'000);
    result = h->sut->append(Batch({Item()}), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok());
    EXPECT_LT(result->front().hlc, (Hlc{10000, 0}));
}
TEST_P(JournalContract, CeilingWaitDoesNotBlockRouteApplication)
{
    h->enableDynamic({100, 0}, 10'000'000'000);
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Accepted); });
    for(int attempt = 0; attempt < 1000 && !h->ceilingWaiting(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(h->ceilingWaiting());
    RouteState state;
    state.route = {8, {{"self", "self:1"}}, "grapher:1", "player:1"};
    auto apply = std::async(std::launch::async, [&] { h->applyRoute(state, false, 10); });
    EXPECT_EQ(apply.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    apply.get();
    h->extendCeiling({10000, 0}, 10'000'000'000);
    ASSERT_EQ(append.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    auto result = append.get();
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsFailedPrecondition(result->front().status));
    EXPECT_EQ(result->front().current_route->epoch, 8);
}
TEST_P(JournalContract, TransitionsCannotRatchetTheAcceptanceClock)
{
    h->enableDynamic({100'000'000'000, 0}, 100'000'000'000);
    for(uint64_t epoch = 8; epoch < 24; epoch += 2)
    {
        RouteState removed;
        removed.route = {epoch, {{"other", "other:1"}}, "grapher:1", ""};
        h->applyRoute(removed, false, epoch * 2);
        RouteState added;
        added.route = {epoch + 1, {{"self", "self:1"}}, "grapher:1", ""};
        added.physical_floor = static_cast<int64_t>(epoch) * 3'000'000'000;
        added.ordering_cut = {static_cast<int64_t>(epoch) * 30'000'000'000, 0};
        h->applyRoute(added, true, epoch * 2 + 1);
        auto result = h->sut->append(Batch({Item()}, epoch + 1), Durability::Accepted);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(absl::IsUnavailable(result->front().status));
        EXPECT_LE(h->acceptanceClock(), 100 + 3'000'000'000);
    }
}
TEST_P(JournalContract, RouteChangeWaitsOutValidatedAdmissions)
{
    h->enableDynamic({10000, 0}, 10'000'000'000);
    std::promise<void> assigned, resume;
    auto ready = assigned.get_future();
    auto unblock = resume.get_future().share();
    h->onAssignment(
            [&](Hlc)
            {
                assigned.set_value();
                unblock.wait_for(std::chrono::seconds(3));
            });
    auto append = std::async(std::launch::async, [&] { return h->sut->append(Batch({Item()}), Durability::Accepted); });
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "grapher:1", ""};
    auto apply = std::async(std::launch::async, [&] { h->applyRoute(state, false, 10); });
    EXPECT_EQ(apply.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    resume.set_value();
    ASSERT_EQ(apply.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    apply.get();
    auto result = append.get();
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result->front().status.ok());
    h->onAssignment(nullptr);
    result = h->sut->append(Batch({Item(2)}, 8), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsFailedPrecondition(result->front().status));
}
TEST_P(JournalContract, NewMemberAcceptanceClockCoversTheStoryPhysicalFloor)
{
    h->enableDynamic({100'000'000'000, 0}, 100'000'000'000);
    RouteState removed;
    removed.route = {8, {{"other", "other:1"}}, "grapher:1", ""};
    h->applyRoute(removed, false, 10);
    RouteState added;
    added.route = {9, {{"self", "self:1"}}, "grapher:1", ""};
    added.physical_floor = 4'000'000'000;
    added.ordering_cut = {5'000'000'000, 0};
    h->applyRoute(added, true, 11);
    auto result = h->sut->append(Batch({Item()}, 9), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsUnavailable(result->front().status));
    h->setPhysical(2'000'000'000);
    auto item = Item();
    item.physical.physical_ns = 4'000'000'000;
    result = h->sut->append(Batch({item}, 9), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    EXPECT_GE(h->acceptanceClock(), added.physical_floor);
    EXPECT_GT(result->front().hlc, added.ordering_cut);
}
TEST_P(JournalContract, UnlistedKeeperRejectsAppend)
{
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "grapher:1", ""};
    h->applyRoute(state, false, 10);
    auto result = h->sut->append(Batch({Item()}, 8), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsFailedPrecondition(result->front().status));
    ASSERT_TRUE(result->front().current_route);
    EXPECT_EQ(result->front().current_route->epoch, 8);
    EXPECT_EQ(eventCount(), 0);
}
TEST_P(JournalContract, RetiredKeeperDrainsWithAnAlreadyEvictedFinalChunk)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "archive drain requires WAL";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->deliverChunk(*chunk, "grapher", 1);
    h->releaseTail();
    h->reportArchive({1, chunk->end, "grapher", 1, {}, false});
    EXPECT_EQ(eventCount(), 0);
    h->enableDynamic({10'000'000'000, 0}, 20'000'000'000);
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "127.0.0.1:1", ""};
    const Hlc cut{2'000'000'000, 0};
    state.predecessors.push_back({{"self", "self:1"}, "instance", 7, cut, 20'000'000'000});
    h->applyRoute(state, false, 10);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    ASSERT_FALSE(chunks->empty());
    EXPECT_EQ(chunks->back().start, chunk->end);
    EXPECT_EQ(chunks->back().end, cut);
    h->deliverChunk(chunks->back(), "grapher", 2);
    h->releaseTail();
    h->reportArchive({1, cut, "grapher", 2, {}, false});
    EXPECT_TRUE(h->retiredDrained());
}
TEST_P(JournalContract, WriterlessRetiredStoryDrainsAtItsOwnCut)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "archive drain requires WAL";
    h->enableDynamic({10'000'000'000, 0}, 20'000'000'000);
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "127.0.0.1:1", ""};
    state.predecessors.push_back({{"self", "self:1"}, "instance", 7, {2'000'000'000, 0}, 20'000'000'000});
    h->applyRoute(state, false, 10);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    EXPECT_TRUE(chunks->empty());
    EXPECT_TRUE(h->retiredDrained());
    EXPECT_EQ(h->evictionFloor(), (Hlc{2'000'000'000, 0}));
}
} // namespace chronolog::contract

namespace chronolog::contract
{
TEST_P(JournalContract, DeferredFloorSurvivesALaterSurvivorUpdate)
{
    h->enableDynamic({100'000'000'000, 0}, 100'000'000'000);
    RouteState state;
    state.route = {8, {{"self", "self:1"}}, "grapher:1", ""};
    state.ordering_cut = {40'000'000'000, 0};
    h->applyRoute(state, true, 10);
    state.route.epoch = 9;
    state.ordering_cut = {500, 0};
    h->applyRoute(state, false, 11);
    auto result = h->sut->append(Batch({Item()}, 9), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(absl::IsUnavailable(result->front().status));
    h->setPhysical(11'000'000'000);
    auto item = Item();
    item.physical.physical_ns = 11'000'000'000;
    result = h->sut->append(Batch({item}, 9), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    EXPECT_GT(result->front().hlc, (Hlc{40'000'000'000, 0}));
}
} // namespace chronolog::contract

namespace chronolog::contract
{
TEST_P(JournalContract, RejoinedKeeperFinishesItsPredecessorEmptyTail)
{
    if(!h->supports_durable)
        GTEST_SKIP() << "archive drain requires WAL";
    auto chunk = archiveChunk();
    ASSERT_TRUE(chunk.ok());
    h->deliverChunk(*chunk, "grapher", 1);
    h->releaseTail();
    h->reportArchive({1, chunk->end, "grapher", 1, {}, false});
    ASSERT_EQ(eventCount(), 0);
    h->enableDynamic({10'000'000'000, 0}, 20'000'000'000);
    RouteState state;
    state.route = {9, {{"self", "self:1"}}, "127.0.0.1:1", ""};
    const Hlc cut{2'500'000'000, 0};
    state.predecessors.push_back({{"self", "self:1"}, "instance", 7, cut, 20'000'000'000});
    h->applyRoute(state, false, 11);
    h->setPhysical(cut.physical_ns);
    auto chunks = h->sealArchive();
    ASSERT_TRUE(chunks.ok());
    ASSERT_FALSE(chunks->empty());
    EXPECT_EQ(chunks->back().start, chunk->end);
    EXPECT_EQ(chunks->back().end, cut);
    h->deliverChunk(chunks->back(), "grapher", 2);
    h->releaseTail();
    h->reportArchive({1, cut, "grapher", 2, {}, false});
    EXPECT_EQ(h->evictionFloor(), cut);
}
TEST_P(JournalContract, WriterCheckpointHeadSurvivesReleaseAndEviction)
{
    auto accepted = h->sut->append(Batch({Item()}), h->supports_durable ? Durability::Durable : Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(accepted->at(0).status.ok());
    auto bad = Item(2);
    bad.physical = {-100'000'000'000LL, 1, ClockStatus::Synced};
    auto rejected = h->sut->append(Batch({bad}), Durability::Accepted);
    ASSERT_TRUE(rejected.ok());
    ASSERT_EQ(rejected->at(0).status.code(), absl::StatusCode::kOutOfRange);
    h->terminateIncarnation(AcquisitionTerminationCause::Expired);
    h->evictEvents();
    auto verify = [&]
    {
        auto status = h->writerStatus({1, 2, 3, 1});
        ASSERT_TRUE(status.ok());
        EXPECT_TRUE(status->known);
        EXPECT_EQ(status->next_sequence, 3u);
        EXPECT_EQ(status->last_hlc, accepted->at(0).hlc);
        EXPECT_TRUE(status->released);
        EXPECT_EQ(status->termination_cause, AcquisitionTerminationCause::Expired);
        ASSERT_TRUE(status->recorded);
        EXPECT_EQ(status->recorded->hlc, accepted->at(0).hlc);
        status = h->writerStatus({1, 2, 3, 2});
        ASSERT_TRUE(status.ok());
        ASSERT_TRUE(status->recorded);
        EXPECT_EQ(status->recorded->status.code(), absl::StatusCode::kOutOfRange);
        status = h->writerStatus({1, 2, 99, 1});
        ASSERT_TRUE(status.ok());
        EXPECT_FALSE(status->known);
    };
    verify();
    if(h->supports_durable)
    {
        h->crashRestart();
        verify();
    }
}

} // namespace chronolog::contract
