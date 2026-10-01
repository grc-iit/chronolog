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
    // Construct the implementation with its production static epoch.
    std::function<std::unique_ptr<Membership>()> staticEpoch;
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
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(MembershipContract);
} // namespace chronolog::contract
