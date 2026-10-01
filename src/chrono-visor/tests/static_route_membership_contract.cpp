// Instantiates MembershipContract for StaticRouteMembership and covers the parts of
// its behavior the generic suite cannot see: heartbeat liveness, applied revisions and
// the Release fence wait.
#include <atomic>
#include <chrono>
#include <thread>

#include "TestSupport.h"
#include "membership/StaticRouteMembership.h"
#include "membership_contract_test.cpp"

namespace chronolog::contract
{
namespace
{

using visor::StaticRouteMembership;
using visor::testing::twoKeeperTopology;
using namespace std::chrono_literals;

std::unique_ptr<StaticRouteMembership>
make(Epoch epoch, std::function<StaticRouteMembership::TimePoint()> now = &std::chrono::steady_clock::now)
{
    return std::make_unique<StaticRouteMembership>(
            twoKeeperTopology(),
            epoch,
            [](StoryId id) { return id == 1; },
            15s,
            std::move(now));
}

MembershipFactory staticFactory()
{
    return []
    {
        auto harness = std::make_unique<MembershipHarness>();
        harness->sut = make(7);
        return harness;
    };
}

std::string paramName(const ::testing::TestParamInfo<MembershipFactory>&) { return "Default"; }

INSTANTIATE_TEST_SUITE_P(Static, MembershipContract, ::testing::Values(staticFactory()), paramName);

TEST(static_route_membership, EpochOneIsServedAndValidated)
{
    auto membership = make(1);
    auto route = membership->route(1);
    ASSERT_TRUE(route.ok());
    EXPECT_EQ(route->epoch, 1u);
    EXPECT_TRUE(membership->validateEpoch(1, 1).ok());
    EXPECT_FALSE(membership->validateEpoch(1, 2).ok());
    EXPECT_EQ(membership->route(2).status().code(), absl::StatusCode::kNotFound);
}

TEST(static_route_membership, AppliedRevisionNeverLowersAndResetsOnNewInstance)
{
    auto membership = make(1);
    const Process keeper{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper};
    ASSERT_TRUE(membership->registerProcess(keeper).ok());
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 5).ok());
    EXPECT_TRUE(membership->waitApplied("keeper-1", 5, 0ms));
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 3).ok());
    EXPECT_TRUE(membership->waitApplied("keeper-1", 5, 0ms));
    EXPECT_FALSE(membership->waitApplied("keeper-1", 6, 0ms));
    EXPECT_FALSE(membership->waitApplied("unregistered", 1, 0ms));

    Process restarted = keeper;
    restarted.instance = "i2";
    ASSERT_TRUE(membership->registerProcess(restarted).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 1, 0ms));
    EXPECT_FALSE(membership->heartbeat("keeper-1", "i1", 9).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 9, 0ms));
}

TEST(static_route_membership, WaitAppliedWakesWhenAHeartbeatArrives)
{
    auto membership = make(1);
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    std::atomic<bool> result{false};
    std::thread waiter([&] { result = membership->waitApplied("keeper-1", 4, 10s); });
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 4).ok());
    waiter.join();
    EXPECT_TRUE(result.load());
}

TEST(static_route_membership, WaitAppliedTimesOutWithoutConfirmation)
{
    auto membership = make(1);
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 1, 30ms));
}

TEST(static_route_membership, LivenessFollowsHeartbeatTimeout)
{
    StaticRouteMembership::TimePoint now{};
    auto membership = make(1, [&now] { return now; });
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    EXPECT_TRUE(membership->alive("keeper-1"));
    now += 14s;
    EXPECT_TRUE(membership->alive("keeper-1"));
    now += 2s;
    EXPECT_FALSE(membership->alive("keeper-1"));
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1").ok());
    EXPECT_TRUE(membership->alive("keeper-1"));
    EXPECT_FALSE(membership->alive("never-registered"));
}

TEST(static_route_membership, RejectsMalformedRegistration)
{
    auto membership = make(1);
    EXPECT_EQ(membership->registerProcess(Process{"", "i", "e", ProcessRole::Keeper}).code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(membership->heartbeat("", "i").code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(membership->heartbeat("unknown", "i").code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace chronolog::contract
