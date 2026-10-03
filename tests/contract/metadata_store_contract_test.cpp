#include <algorithm>
// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/metadata_store.h"
#include "chronolog/acquire_refusal.h"
namespace chronolog::contract
{
// Fence confirmations default to enabled; the dedicated release test changes delivery.
// restart replaces sut with a new instance opening the same durable state.
enum class AuthorityClockMode
{
    Ticking,
    Jump
};
struct MetadataStoreHarness
{
    std::unique_ptr<MetadataStore> sut;
    // Control only delivery of Keeper fence confirmation; timeout is configured
    // short in the fixture. Does not mutate Catalog commit or result semantics.
    std::function<void(bool)> confirmReleaseFence;
    std::function<void()> restart;
    // Whether restart reopens durable state rather than retaining a RAM double.
    bool durable_restart{};
    // G2 factories explicitly select static proof semantics when enabling leases.
    bool static_fence_proof;
    bool acquisition_leases{};
    std::function<void(int64_t, AuthorityClockMode)> advanceAuthorityClock;
    std::function<absl::Status()> stepExpirySweep;
    // Optional dispatch slots until G2 adds the implemented contract methods.
    std::function<absl::StatusOr<Acquisition>(StoryId, std::string, AcquireOptions)> acquireWithOptions;
    std::function<absl::StatusOr<std::vector<RenewAcquisitionResult>>(const std::vector<RenewAcquisition>&)>
            renewAcquisitions;
    int64_t lease_default_ns{}, lease_min_ns{}, lease_max_ns{};
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

// G1 stages these non-enforcement gates; G2 enables every store factory.
TEST_P(MetadataStoreContract, EveryAcquisitionHasAFiniteLease)
{
    if(!h->acquisition_leases)
        GTEST_SKIP() << "RFC-G G1 staging; enabled by G2";
    auto grant = h->sut->acquire(h->story, "finite");
    ASSERT_TRUE(grant.ok()) << grant.status();
    EXPECT_GT(grant->lease.duration_ns, 0);
    EXPECT_GE(grant->lease.remaining_ns, 0);
    EXPECT_LE(grant->lease.remaining_ns, grant->lease.duration_ns);
    EXPECT_FALSE(grant->keeper_preference);
}

TEST_P(MetadataStoreContract, LeaseRequestUsesDefaultAndClamps)
{
    if(!h->acquisition_leases)
        GTEST_SKIP() << "RFC-G G1 staging; enabled by G2";
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_GT(h->lease_min_ns, 1);
    ASSERT_LE(h->lease_min_ns, h->lease_default_ns);
    ASSERT_LE(h->lease_default_ns, h->lease_max_ns);
    for(auto requested: {std::optional<int64_t>{}, std::optional<int64_t>{1}, std::optional<int64_t>{INT64_MAX}})
    {
        AcquireOptions options;
        options.lease_duration_ns = requested;
        options.acquire_request_id = requested ? "duration-request-0000000000000001" + std::to_string(*requested)
                                               : "default-request-0000000000000001";
        auto grant = h->acquireWithOptions(h->story, options.acquire_request_id, options);
        ASSERT_TRUE(grant.ok()) << grant.status();
        EXPECT_EQ(grant->lease.duration_ns,
                  requested ? std::clamp(*requested, h->lease_min_ns, h->lease_max_ns) : h->lease_default_ns);
    }
    for(int64_t duration: {0, -1})
    {
        AcquireOptions options;
        options.lease_duration_ns = duration;
        options.acquire_request_id = "invalid-duration-request-00000001";
        auto grant = h->acquireWithOptions(h->story, "invalid-duration", options);
        EXPECT_TRUE(absl::IsInvalidArgument(grant.status()));
    }
}

TEST_P(MetadataStoreContract, RetriedAcquireAfterLostReplyReturnsTheSameGrant)
{
    if(!h->acquisition_leases)
        GTEST_SKIP() << "RFC-G G1 staging; enabled by G2";
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_TRUE(h->advanceAuthorityClock);
    AcquireOptions options;
    options.acquire_request_id = "lost-reply-request-00000000000001";
    auto first = h->acquireWithOptions(h->story, "retry", options);
    ASSERT_TRUE(first.ok()) << first.status();
    h->advanceAuthorityClock(first->lease.duration_ns / 4, AuthorityClockMode::Ticking);
    auto retry = h->acquireWithOptions(h->story, "retry", options);
    ASSERT_TRUE(retry.ok()) << retry.status();
    EXPECT_EQ(retry->writer_id, first->writer_id);
    EXPECT_EQ(retry->incarnation, first->incarnation);
    EXPECT_EQ(retry->route, first->route);
    EXPECT_EQ(retry->assigned_keeper, first->assigned_keeper);
    EXPECT_EQ(retry->lease.duration_ns, first->lease.duration_ns);
    EXPECT_LT(retry->lease.remaining_ns, first->lease.remaining_ns);
}

TEST_P(MetadataStoreContract, SameIdTerminalRetryReportsCauseAndMatchedIncarnation)
{
    if(!h->acquisition_leases)
        GTEST_SKIP() << "RFC-G G1 staging; enabled by G2";
    ASSERT_TRUE(h->acquireWithOptions);
    AcquireOptions options;
    options.acquire_request_id = "terminal-retry-request-0000000001";
    auto first = h->acquireWithOptions(h->story, "terminal", options);
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(h->sut->release(h->story, first->writer_id, first->incarnation).ok());
    auto retry = h->acquireWithOptions(h->story, "terminal", options);
    ASSERT_TRUE(absl::IsFailedPrecondition(retry.status()));
    auto refusal = getAcquireRefusal(retry.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->matched_incarnation, first->incarnation);
    EXPECT_EQ(refusal->termination_cause, AcquisitionTerminationCause::Released);
    EXPECT_FALSE(refusal->current_incarnation);
}

TEST_P(MetadataStoreContract, RestartRetainsDurationAndTerminalCause)
{
    if(!h->acquisition_leases)
        GTEST_SKIP() << "RFC-G G1 staging; enabled by G2";
    ASSERT_TRUE(h->acquireWithOptions);
    ASSERT_TRUE(h->renewAcquisitions);
    ASSERT_TRUE(h->restart);
    AcquireOptions options;
    options.acquire_request_id = "restart-request-00000000000000001";
    auto first = h->acquireWithOptions(h->story, "restart", options);
    ASSERT_TRUE(first.ok()) << first.status();
    RenewAcquisition tuple{h->story, first->writer_id, first->incarnation};
    h->restart();
    auto live = h->renewAcquisitions({tuple});
    ASSERT_TRUE(live.ok()) << live.status();
    ASSERT_EQ(live->size(), 1u);
    ASSERT_TRUE(live->front().status.ok());
    ASSERT_TRUE(live->front().lease);
    EXPECT_EQ(live->front().lease->duration_ns, first->lease.duration_ns);
    ASSERT_TRUE(h->sut->release(tuple.story_id, tuple.writer_id, tuple.incarnation).ok());
    h->restart();
    auto terminal = h->renewAcquisitions({tuple});
    ASSERT_TRUE(terminal.ok()) << terminal.status();
    ASSERT_EQ(terminal->size(), 1u);
    EXPECT_TRUE(absl::IsFailedPrecondition(terminal->front().status));
    EXPECT_EQ(terminal->front().termination_cause, AcquisitionTerminationCause::Released);
    EXPECT_FALSE(terminal->front().lease);
}

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
    auto replacement = h->sut->createStory("c", "s");
    ASSERT_TRUE(replacement.ok());
    EXPECT_NE(replacement->id, h->story);
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
TEST_P(MetadataStoreContract, RevisionSurvivesRestart)
{
    if(!h->durable_restart)
        GTEST_SKIP() << "in-memory store has no durable restart";
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    auto first = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(first.ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    auto second = h->sut->release(h->story, b->writer_id, b->incarnation);
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->revision, first->revision);
    auto retry = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, first->revision);
}
TEST_P(MetadataStoreContract, RevisionSurvivesLeaderChange)
{
    if(!h->durable_restart)
        GTEST_SKIP() << "in-memory store has no durable restart";
    ASSERT_TRUE(h->restart);
    auto a = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(a.ok());
    auto first = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(first.ok());
    h->restart();
    auto b = h->sut->acquire(h->story, "writer");
    ASSERT_TRUE(b.ok());
    auto second = h->sut->release(h->story, b->writer_id, b->incarnation);
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second->revision, first->revision);
    auto retry = h->sut->release(h->story, a->writer_id, a->incarnation);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, first->revision);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MetadataStoreContract);
} // namespace chronolog::contract
