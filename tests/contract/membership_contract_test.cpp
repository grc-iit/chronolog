// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/membership.h"
namespace chronolog::contract
{
// Factory installs story 1 at epoch 7 with keeper, grapher and player endpoints.
struct MembershipHarness
{
    std::unique_ptr<Membership> sut;
    std::function<absl::Status(uint64_t)> registerPolicy;
    // Construct the implementation with its production static epoch.
    std::function<std::unique_ptr<Membership>()> staticEpoch;
    std::function<absl::Status(std::string, std::string)> grantCeiling;
    std::function<absl::Status(std::string)> drainKeeper, joinKeeper, abandonKeeper;
    std::function<absl::Status(std::string, std::string, Epoch, Hlc)> reportDrain;
    std::function<absl::Status(std::string, std::string, Hlc, Hlc, Hlc)> reportSettlement;
    // Catalog side of story destroy (W10.17); empty when the implementation owns no Catalog.
    struct RouteDelta
    {
        StoryId story_id{};
        uint64_t revision{};
        bool tombstoned{};
    };
    std::function<absl::Status()> destroyStory;
    std::function<uint64_t()> revision;
    // Updates with revision above the cursor in revision order, as WatchRoutes would deliver them.
    std::function<std::vector<RouteDelta>(uint64_t)> deltasSince;
    std::function<Epoch()> storyEpoch;
};
using MembershipFactory = std::function<std::unique_ptr<MembershipHarness>()>;
class MembershipContract: public ::testing::TestWithParam<MembershipFactory>
{
protected:
    std::unique_ptr<MembershipHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
    }
};

TEST_P(MembershipContract, MismatchedPolicyRefusesRegistration)
{
    if(!h->registerPolicy)
        GTEST_SKIP() << "wire registration is covered by cluster_adapter";
    EXPECT_EQ(h->registerPolicy(2).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(h->registerPolicy(1).ok());
}

TEST_P(MembershipContract, RouteSnapshotContainsAllRoles)
{
    auto r = h->sut->route(1);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->epoch, 7u);
    EXPECT_FALSE(r->keepers.empty());
    EXPECT_FALSE(r->grapher.empty());
    EXPECT_FALSE(r->player.empty());
}

TEST_P(MembershipContract, StaleEpochRejection)
{
    EXPECT_TRUE(h->sut->validateEpoch(1, 7).ok());
    auto s = h->sut->validateEpoch(1, 6);
    EXPECT_EQ(s.code(), absl::StatusCode::kFailedPrecondition);
}

TEST_P(MembershipContract, UnknownStoryCannotValidate)
{
    EXPECT_FALSE(h->sut->route(999).ok());
    EXPECT_FALSE(h->sut->validateEpoch(999, 7).ok());
}

TEST_P(MembershipContract, RegisterHeartbeatAndRestartFencing)
{
    Process p{"keeper-test", "instance-1", "localhost:1", ProcessRole::Keeper};
    ASSERT_TRUE(h->sut->registerProcess(p).ok());
    EXPECT_TRUE(h->sut->heartbeat(p.id, p.instance).ok());
    p.instance = "instance-2";
    ASSERT_TRUE(h->sut->registerProcess(p).ok());
    EXPECT_FALSE(h->sut->heartbeat(p.id, "instance-1").ok());
    EXPECT_TRUE(h->sut->heartbeat(p.id, p.instance).ok());
}

TEST_P(MembershipContract, StaticEpochIsOneAndValidated)
{
    if(!h->staticEpoch)
        GTEST_SKIP() << "dynamic membership has no static epoch configuration";
    auto membership = h->staticEpoch();
    auto route = membership->route(1);
    ASSERT_TRUE(route.ok());
    EXPECT_EQ(route->epoch, 1u);
    EXPECT_EQ(membership->validateEpoch(1, 1).code(), absl::StatusCode::kOk);
    for(Epoch epoch: {0, 2, 7})
        EXPECT_EQ(membership->validateEpoch(1, epoch).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(membership->route(999).status().code(), absl::StatusCode::kNotFound);
}
TEST_P(MembershipContract, StaleDrainReportDoesNotRemoveANewerPredecessor)
{
    if(!h->drainKeeper)
        GTEST_SKIP() << "dynamic membership only";
    auto initial = h->sut->route(1);
    ASSERT_TRUE(initial.ok());
    ASSERT_GE(initial->keepers.size(), 2u);
    auto a = initial->keepers[0], b = initial->keepers[1];
    ASSERT_TRUE(h->sut->registerProcess({a.process_id, "old-1", a.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->sut->registerProcess({b.process_id, "survivor", b.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->grantCeiling(a.process_id, "old-1").ok());
    ASSERT_TRUE(h->grantCeiling(b.process_id, "survivor").ok());
    ASSERT_TRUE(h->drainKeeper(a.process_id).ok());
    auto retired = h->sut->routeState(1);
    ASSERT_TRUE(retired.ok());
    ASSERT_EQ(retired->predecessors.size(), 1u);
    auto predecessor = retired->predecessors.front();
    ASSERT_TRUE(h->joinKeeper(a.process_id).ok());
    ASSERT_TRUE(h->grantCeiling(a.process_id, "old-1").ok());
    ASSERT_TRUE(h->drainKeeper(a.process_id).ok());
    auto before = h->sut->routeState(1);
    ASSERT_TRUE(before.ok());
    ASSERT_EQ(before->predecessors.size(), 2u);
    ASSERT_TRUE(h->reportDrain(a.process_id, "old-1", predecessor.epoch, predecessor.own_cut).ok());
    auto after = h->sut->routeState(1);
    ASSERT_TRUE(after.ok());
    ASSERT_EQ(after->predecessors.size(), 1u);
    EXPECT_NE(after->predecessors.front().epoch, predecessor.epoch);
    EXPECT_EQ(after->archived_below, predecessor.own_cut);
    ASSERT_TRUE(h->reportDrain(a.process_id, "old-1", predecessor.epoch, predecessor.own_cut).ok());
    after = h->sut->routeState(1);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->predecessors.size(), 1u);
}
TEST_P(MembershipContract, AbandonedRangeUsesOnlyTheSameInstanceProof)
{
    if(!h->abandonKeeper)
        GTEST_SKIP() << "dynamic membership only";
    auto initial = h->sut->route(1);
    ASSERT_TRUE(initial.ok());
    ASSERT_GE(initial->keepers.size(), 2u);
    auto a = initial->keepers[0], b = initial->keepers[1];
    ASSERT_TRUE(h->sut->registerProcess({a.process_id, "old-1", a.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->sut->registerProcess({b.process_id, "survivor", b.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->grantCeiling(a.process_id, "old-1").ok());
    ASSERT_TRUE(h->grantCeiling(b.process_id, "survivor").ok());
    ASSERT_TRUE(h->reportSettlement(a.process_id, "old-1", Hlc{}, Hlc{50, 0}, Hlc{10, 0}).ok());
    ASSERT_TRUE(h->sut->registerProcess({a.process_id, "new-2", a.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->grantCeiling(a.process_id, "new-2").ok());
    ASSERT_TRUE(h->abandonKeeper(a.process_id).ok());
    auto state = h->sut->routeState(1);
    ASSERT_TRUE(state.ok());
    ASSERT_EQ(state->abandoned.size(), 2u);
    EXPECT_TRUE(state->predecessors.empty());
    EXPECT_EQ(state->abandoned[0].start, Hlc{});
    EXPECT_EQ(state->abandoned[1].start, (Hlc{50, 0}));
    EXPECT_EQ(state->archived_below, (Hlc{50, 0}));
}
TEST_P(MembershipContract, DestroyEmitsTombstoneDelta)
{
    if(!h->destroyStory)
        GTEST_SKIP() << "no Catalog behind this membership";
    const auto cursor = h->revision();
    ASSERT_TRUE(h->destroyStory().ok());
    const auto deltas = h->deltasSince(cursor);
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].story_id, 1u);
    EXPECT_TRUE(deltas[0].tombstoned);
    EXPECT_GT(deltas[0].revision, cursor);
    EXPECT_EQ(deltas[0].revision, h->revision());
    EXPECT_FALSE(h->sut->route(1).ok());
    // Destroying again changes nothing and emits no second tombstone.
    ASSERT_TRUE(h->destroyStory().ok());
    EXPECT_EQ(h->deltasSince(cursor).size(), 1u);
}

TEST_P(MembershipContract, NoRouteChangeAfterTombstone)
{
    if(!h->destroyStory || !h->drainKeeper)
        GTEST_SKIP() << "no Catalog or dynamic membership behind this membership";
    auto initial = h->sut->route(1);
    ASSERT_TRUE(initial.ok());
    ASSERT_GE(initial->keepers.size(), 2u);
    auto a = initial->keepers[0], b = initial->keepers[1];
    ASSERT_TRUE(h->sut->registerProcess({a.process_id, "i-a", a.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->sut->registerProcess({b.process_id, "i-b", b.endpoint, ProcessRole::Keeper}).ok());
    ASSERT_TRUE(h->grantCeiling(a.process_id, "i-a").ok());
    ASSERT_TRUE(h->grantCeiling(b.process_id, "i-b").ok());
    ASSERT_TRUE(h->destroyStory().ok());
    const auto epoch = h->storyEpoch();
    const auto cursor = h->revision();
    // A route change for a Keeper that listed the story still commits for its live stories, and skips this one.
    ASSERT_TRUE(h->drainKeeper(a.process_id).ok());
    ASSERT_TRUE(h->joinKeeper(a.process_id).ok());
    for(const auto& delta: h->deltasSince(cursor))
    {
        if(delta.story_id == 1)
        {
            EXPECT_TRUE(delta.tombstoned) << "revision " << delta.revision;
        }
    }
    EXPECT_EQ(h->storyEpoch(), epoch);
    EXPECT_FALSE(h->sut->routeState(1).ok());
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MembershipContract);
} // namespace chronolog::contract
