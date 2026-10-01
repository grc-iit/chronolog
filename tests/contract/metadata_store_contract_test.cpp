#include <algorithm>
// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/metadata_store.h"
namespace chronolog::contract
{
// Fence confirmations default to enabled; the dedicated release test changes delivery.
// restart replaces sut with a new instance opening the same durable state.
struct MetadataStoreHarness
{
    std::unique_ptr<MetadataStore> sut;
    // Control only delivery of Keeper fence confirmation; timeout is configured
    // short in the fixture. Does not mutate Catalog commit or result semantics.
    std::function<void(bool)> confirmReleaseFence;
    std::function<void()> restart;
    StoryId story{};
};
using MetadataStoreFactory = std::function<std::unique_ptr<MetadataStoreHarness>()>;
class MetadataStoreContract: public ::testing::TestWithParam<MetadataStoreFactory>
{
protected:
    std::unique_ptr<MetadataStoreHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
        auto c = h->sut->createChronicle("c");
        ASSERT_TRUE(c.ok());
        auto s = h->sut->createStory("c", "s");
        ASSERT_TRUE(s.ok());
        h->story = s->id;
    }
};

TEST_P(MetadataStoreContract, ChronicleAndStoryCrud)
{
    auto c = h->sut->getChronicle("c");
    ASSERT_TRUE(c.ok());
    EXPECT_FALSE(c->tombstoned);
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s->name, "s");
    auto cs = h->sut->listChronicles();
    ASSERT_TRUE(cs.ok());
    EXPECT_EQ(cs->size(), 1u);
    auto ss = h->sut->listStories("c");
    ASSERT_TRUE(ss.ok());
    EXPECT_EQ(ss->size(), 1u);
}

TEST_P(MetadataStoreContract, AssignedIdsDoNotAliasConcatenatedNames)
{
    ASSERT_TRUE(h->sut->createChronicle("a").ok());
    ASSERT_TRUE(h->sut->createChronicle("ab").ok());
    auto a = h->sut->createStory("a", "bc");
    auto b = h->sut->createStory("ab", "c");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(a->id, b->id);
}

TEST_P(MetadataStoreContract, PersistedIncarnationStrictlyIncreases)
{
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b->writer_id, a->writer_id);
    EXPECT_GT(b->incarnation, a->incarnation);
}

TEST_P(MetadataStoreContract, AcquireReturnsRouteAndEpoch)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a->story_id, h->story);
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(a->route.epoch, s->epoch);
    EXPECT_FALSE(a->assigned_keeper.process_id.empty());
    EXPECT_NE(std::find(a->route.keepers.begin(), a->route.keepers.end(), a->assigned_keeper), a->route.keepers.end());
}

TEST_P(MetadataStoreContract, OldReleaseCannotReleaseNewIncarnation)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    EXPECT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    EXPECT_TRUE(h->sut->release(h->story, b->writer_id, b->incarnation).ok());
}

TEST_P(MetadataStoreContract, StoryTombstonePermanence)
{
    ASSERT_TRUE(h->restart);
    ASSERT_TRUE(h->sut->destroyStory(h->story).ok());
    h->restart();
    auto replacement=h->sut->createStory("c","s"); ASSERT_TRUE(replacement.ok()); EXPECT_NE(replacement->id,h->story);
    EXPECT_FALSE(h->sut->acquire(h->story, "writer").ok());
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s->tombstoned);
}

TEST_P(MetadataStoreContract, ChronicleTombstonePermanence)
{
    ASSERT_TRUE(h->sut->destroyChronicle("c").ok());
    EXPECT_TRUE(h->sut->createChronicle("c").ok());
    EXPECT_TRUE(h->sut->createStory("c", "new").ok());
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s->tombstoned);
}

TEST_P(MetadataStoreContract, EpochCompareAndSet)
{
    auto s = h->sut->getStory(h->story);
    ASSERT_TRUE(s.ok());
    auto a = h->sut->compareAndSetEpoch(h->story, s->epoch, s->epoch + 1);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(*a, s->epoch + 1);
    EXPECT_FALSE(h->sut->compareAndSetEpoch(h->story, s->epoch, s->epoch + 2).ok());
}

TEST_P(MetadataStoreContract, DestroyRefusesActiveAcquisitions)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(h->sut->destroyStory(h->story).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(h->sut->destroyChronicle("c").code(), absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    EXPECT_TRUE(h->sut->destroyChronicle("c").ok());
}

TEST_P(MetadataStoreContract, EpochMustStrictlyIncrease)
{
    auto story = h->sut->getStory(h->story);
    ASSERT_TRUE(story.ok());
    EXPECT_EQ(h->sut->compareAndSetEpoch(h->story, story->epoch, story->epoch).status().code(),
              absl::StatusCode::kInvalidArgument);
}

TEST_P(MetadataStoreContract, AssignedKeeperStableWithinWriterEpoch)
{
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    ASSERT_FALSE(a->assigned_keeper.process_id.empty());
    ASSERT_TRUE(h->sut->release(h->story, a->writer_id, a->incarnation).ok());
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    ASSERT_EQ(a->route.epoch, b->route.epoch);
    EXPECT_EQ(a->writer_id, b->writer_id);
    EXPECT_EQ(a->assigned_keeper, b->assigned_keeper);
}

TEST_P(MetadataStoreContract, ReleaseReportsFenceState)
{
    ASSERT_TRUE(h->confirmReleaseFence);
    h->confirmReleaseFence(false);
    auto first = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(first.ok());
    auto pending = h->sut->release(h->story, first->writer_id, first->incarnation);
    ASSERT_TRUE(pending.ok());
    EXPECT_FALSE(pending->fenced);
    EXPECT_GT(pending->revision, 0u);
    auto second = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->incarnation, first->incarnation);
    h->confirmReleaseFence(true);
    auto confirmed = h->sut->release(h->story, second->writer_id, second->incarnation);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_TRUE(confirmed->fenced);
    EXPECT_GT(confirmed->revision, pending->revision);
}
TEST_P(MetadataStoreContract, RevisionSurvivesRestart) {
 ASSERT_TRUE(h->restart); auto a=h->sut->acquire(h->story,"writer"); ASSERT_TRUE(a.ok()); auto first=h->sut->release(h->story,a->writer_id,a->incarnation); ASSERT_TRUE(first.ok()); h->restart();
 auto b=h->sut->acquire(h->story,"writer"); ASSERT_TRUE(b.ok()); auto second=h->sut->release(h->story,b->writer_id,b->incarnation); ASSERT_TRUE(second.ok()); EXPECT_GT(second->revision,first->revision);
 auto retry=h->sut->release(h->story,a->writer_id,a->incarnation); ASSERT_TRUE(retry.ok()); EXPECT_EQ(retry->revision,first->revision);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MetadataStoreContract);
} // namespace chronolog::contract
