#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "ram_harness.h"

namespace chronolog
{
namespace
{

AppendItem Item(uint64_t sequence, uint64_t writer = 2, uint64_t incarnation = 3)
{
    AppendItem i;
    i.writer_id = writer;
    i.incarnation = incarnation;
    i.sequence = sequence;
    i.physical = {100, 1, ClockStatus::Synced};
    i.envelope.payload = "event";
    return i;
}

AppendBatch Batch(std::vector<AppendItem> items, Epoch epoch = 7) { return {1, epoch, std::move(items)}; }

Range All() { return {Range::Axis::Hlc, {0, 0}, {INT64_MAX, 0}}; }

} // namespace

TEST(RamJournal, DurableAndUnspecifiedAreUnimplementedAndStoreNothing)
{
    test::RamRig rig;
    for(auto d: {Durability::Durable, Durability::Unspecified})
    {
        auto r = rig.journal->append(Batch({Item(1)}), d);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ((*r)[0].status.code(), absl::StatusCode::kUnimplemented);
        EXPECT_EQ((*r)[0].achieved, Durability::Unspecified);
    }
    auto events = rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    EXPECT_TRUE(events->empty());
}

TEST(RamJournal, StaleEpochFailsEveryItemWithRoute)
{
    test::RamRig rig;
    auto r = rig.journal->append(Batch({Item(1), Item(2)}, 6), Durability::Accepted);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r->size(), 2u);
    for(const auto& item: *r)
    {
        EXPECT_EQ(item.status.code(), absl::StatusCode::kFailedPrecondition);
        ASSERT_TRUE(item.current_route);
        EXPECT_EQ(item.current_route->epoch, 7u);
    }
}

TEST(RamJournal, UnknownStoryIsNotFound)
{
    test::RamRig rig;
    EXPECT_EQ(rig.journal->read(9, All()).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ(rig.journal->keeperFrontier(9).status().code(), absl::StatusCode::kNotFound);
    AppendBatch b{9, 7, {Item(1)}};
    auto r = rig.journal->append(b, Durability::Accepted);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ((*r)[0].status.code(), absl::StatusCode::kNotFound);
}

TEST(RamJournal, UnregisteredWriterIsRejectedWithRoute)
{
    test::RamRig rig;
    auto r = rig.journal->append(Batch({Item(1, 9, 1)}), Durability::Accepted);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ((*r)[0].status.code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE((*r)[0].current_route);
}

TEST(RamJournal, SupersededIncarnationEventsStayReadableAndNewOneSortsAfter)
{
    test::RamRig rig;
    ASSERT_TRUE((*rig.journal->append(Batch({Item(1)}), Durability::Accepted))[0].status.ok());
    ASSERT_TRUE(rig.journal->registerWriter(1, 2, 4).ok());
    EXPECT_EQ(rig.journal->registerWriter(1, 2, 3).code(), absl::StatusCode::kFailedPrecondition);
    auto r = rig.journal->append(Batch({Item(1, 2, 4)}), Durability::Accepted);
    ASSERT_TRUE((*r)[0].status.ok());
    auto events = rig.journal->read(1, All());
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 2u);
    EXPECT_EQ((*events)[0].id.incarnation, 3u);
    EXPECT_EQ((*events)[1].id.incarnation, 4u);
    auto f = rig.journal->frontier(1);
    ASSERT_TRUE(f.ok());
    ASSERT_EQ(f->size(), 1u);
    EXPECT_EQ((*f)[0].incarnation, 4u);
}

TEST(RamJournal, OlderReleaseDoesNotFenceNewerIncarnation)
{
    test::RamRig rig;
    ASSERT_TRUE(rig.journal->registerWriter(1, 2, 4).ok());
    rig.journal->releaseWriter(1, 2, 3);
    auto r = rig.journal->append(Batch({Item(1, 2, 4)}), Durability::Accepted);
    EXPECT_TRUE((*r)[0].status.ok());
}

TEST(RamJournal, DedupeWindowEvictsOldResults)
{
    RamJournalConfig config;
    config.dedupe_window = 2;
    test::RamRig rig(config);
    auto first = rig.journal->append(Batch({Item(1), Item(2), Item(3)}), Durability::Accepted);
    ASSERT_TRUE(first.ok());
    auto again = rig.journal->append(Batch({Item(3), Item(2), Item(1)}), Durability::Accepted);
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE((*again)[0].status.ok());
    EXPECT_EQ((*again)[0].hlc, (*first)[2].hlc);
    EXPECT_EQ((*again)[1].hlc, (*first)[1].hlc);
    EXPECT_EQ((*again)[2].status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(RamJournal, FrontierExceedsEveryAssignedHlcAcrossWriters)
{
    test::RamRig rig;
    ASSERT_TRUE(rig.journal->registerWriter(1, 5, 1).ok());
    auto r = rig.journal->append(Batch({Item(1), Item(1, 5, 1), Item(2)}), Durability::Accepted);
    ASSERT_TRUE(r.ok());
    auto seal = rig.journal->keeperFrontier(1);
    ASSERT_TRUE(seal.ok());
    for(const auto& item: *r) EXPECT_GT(*seal, item.hlc);
    auto next = rig.journal->append(Batch({Item(3)}), Durability::Accepted);
    EXPECT_GT((*next)[0].hlc, *seal);
}

// A sealed frontier must already cover every event assigned below it, so a read of [0, F) taken right after
// the seal never changes while writers keep appending. Every loop here has a fixed bound.
} // namespace chronolog
