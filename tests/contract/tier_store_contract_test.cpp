// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/tier_store.h"
namespace chronolog::contract
{
// A fresh store is anchored at HLC {100,0}. Fault injection affects the next
// publish only. restart reopens the durable archive. eraseFile appends deletion.
// injectTornRecord appends incomplete manifest bytes; writerLogs exposes log ids.
// tombstone appends the durable story-tombstone record (I13.11).
struct TierStoreHarness
{
    std::unique_ptr<TierStore> sut;
    std::function<void()> restart, failNextPublish, injectTornRecord;
    std::function<void(std::string)> eraseFile;
    std::function<void(StoryId)> tombstone;
    std::function<std::vector<std::string>()> writerLogs;
    std::function<absl::StatusOr<ManifestRecord>(Chunk)> publishOtherWriter;
};
Chunk Window(int64_t start = 100, int64_t end = 200, bool empty = false)
{
    Chunk c;
    c.id = std::to_string(start);
    c.story_id = 1;
    c.start = {start, 0};
    c.end = {end, 0};
    if(!empty)
    {
        Event e;
        e.id = {1, 2, 3, static_cast<uint64_t>(start)};
        e.hlc = {start, 1};
        e.envelope.payload = "data";
        c.events.push_back(e);
    }
    return c;
}
Range WholeArchive() { return {Range::Axis::Hlc, {0, 0}, {1000, 0}}; }

using TierStoreFactory = std::function<std::unique_ptr<TierStoreHarness>()>;
class TierStoreContract: public ::testing::TestWithParam<TierStoreFactory>
{
protected:
    std::unique_ptr<TierStoreHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
    }
};

TEST_P(TierStoreContract, PublishRecordNamesVisibleCompleteFile)
{
    auto a = h->sut->publish(Window());
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->state, ManifestState::Published);
    EXPECT_EQ(a->event_count, 1u);
    auto e = h->sut->read(1, WholeArchive());
    ASSERT_TRUE(e.ok());
    ASSERT_EQ(e->size(), 1u);
    EXPECT_EQ((*e)[0].envelope.payload, "data");
}

TEST_P(TierStoreContract, WatermarkCannotJumpGap)
{
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    ASSERT_TRUE(h->sut->publish(Window(300, 400)).ok());
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
    ASSERT_TRUE(h->sut->publish(Window(200, 300)).ok());
    w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{400, 0}));
}

TEST_P(TierStoreContract, WatermarkNeverRegresses)
{
    ASSERT_TRUE(h->eraseFile);
    ASSERT_TRUE(h->restart);
    auto published = h->sut->publish(Window());
    ASSERT_TRUE(published.ok());
    ASSERT_TRUE(h->sut->publish(Window(200, 300, true)).ok());
    auto watermark = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(watermark.ok());
    ASSERT_EQ(*watermark, (Hlc{300, 0}));
    auto check = [&]
    {
        auto next = h->sut->contiguousWatermark(1);
        ASSERT_TRUE(next.ok());
        EXPECT_GE(*next, *watermark);
        watermark = next;
    };
    auto overlap = Window(100, 150);
    overlap.id = "overlap";
    ASSERT_TRUE(h->sut->publish(overlap).ok());
    check();
    ASSERT_TRUE(h->failNextPublish);
    h->failNextPublish();
    auto failed = h->sut->publish(Window(300, 400));
    EXPECT_TRUE(!failed.ok() || failed->state == ManifestState::Failed);
    check();
    h->eraseFile(published->file);
    check();
    h->restart();
    check();
}

TEST_P(TierStoreContract, EmptyWindowMaintainsContinuity)
{
    auto a = h->sut->publish(Window(100, 200, true));
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->state, ManifestState::Empty);
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
}

TEST_P(TierStoreContract, ExemptSalvageReadableWithoutAdvancingWatermark)
{
    auto c = Window();
    c.exempt = true;
    ASSERT_TRUE(h->sut->publish(c).ok());
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_LE(*w, (Hlc{100, 0}));
    auto e = h->sut->read(1, WholeArchive());
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e->size(), 1u);
}

TEST_P(TierStoreContract, FailedWriteNeverAdvancesWatermark)
{
    ASSERT_TRUE(h->failNextPublish);
    h->failNextPublish();
    auto p = h->sut->publish(Window());
    EXPECT_TRUE(!p.ok() || p->state == ManifestState::Failed);
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_LE(*w, (Hlc{100, 0}));
}

TEST_P(TierStoreContract, ManifestRestoresContiguousWatermark)
{
    ASSERT_TRUE(h->restart);
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    h->restart();
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
}

TEST_P(TierStoreContract, TornManifestRecordIsIgnored)
{
    ASSERT_TRUE(h->restart);
    ASSERT_TRUE(h->injectTornRecord);
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    h->injectTornRecord();
    h->restart();
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
}

TEST_P(TierStoreContract, RotationsNeverOverwriteAndIdentityDeduplicates)
{
    auto a = h->sut->publish(Window());
    auto b = h->sut->publish(Window());
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    auto e = h->sut->read(1, WholeArchive());
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e->size(), 1u);
    auto m = h->sut->manifest(1);
    ASSERT_TRUE(m.ok());
    EXPECT_FALSE(m->empty());
}

TEST_P(TierStoreContract, DeletedFileSupersedesPublishedRecord)
{
    ASSERT_TRUE(h->eraseFile);
    auto a = h->sut->publish(Window());
    ASSERT_TRUE(a.ok());
    h->eraseFile(a->file);
    auto e = h->sut->read(1, WholeArchive());
    ASSERT_TRUE(e.ok());
    EXPECT_TRUE(e->empty());
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
}

TEST_P(TierStoreContract, TombstoneRecordRefusesLatePublish)
{
    ASSERT_TRUE(h->tombstone);
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    h->tombstone(1);
    auto late = h->sut->publish(Window(200, 300));
    ASSERT_FALSE(late.ok());
    EXPECT_EQ(late.status().code(), absl::StatusCode::kFailedPrecondition);
    h->restart();
    late = h->sut->publish(Window(200, 300));
    ASSERT_FALSE(late.ok());
    EXPECT_EQ(late.status().code(), absl::StatusCode::kFailedPrecondition);
    auto manifest = h->sut->manifest(1);
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest->size(), 1u);
    auto events = h->sut->read(1, WholeArchive());
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{200, 0}));
}

TEST_P(TierStoreContract, HalfOpenRead)
{
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    Range r{Range::Axis::Hlc, {0, 0}, {100, 1}};
    auto e = h->sut->read(1, r);
    ASSERT_TRUE(e.ok());
    EXPECT_TRUE(e->empty());
}

TEST_P(TierStoreContract, IndependentWriterLogsMergeWithoutSharedAppend)
{
    ASSERT_TRUE(h->publishOtherWriter);
    ASSERT_TRUE(h->writerLogs);
    ASSERT_TRUE(h->sut->publish(Window()).ok());
    ASSERT_TRUE(h->publishOtherWriter(Window(200, 300)).ok());
    auto logs = h->writerLogs();
    ASSERT_GE(logs.size(), 2u);
    EXPECT_NE(logs[0], logs[1]);
    ASSERT_TRUE(h->restart);
    h->restart();
    auto w = h->sut->contiguousWatermark(1);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(*w, (Hlc{300, 0}));
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TierStoreContract);
} // namespace chronolog::contract
