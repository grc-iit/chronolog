// Reusable suite: include this .cpp in the implementation instantiation TU,
// provide the documented fresh harness factory, then INSTANTIATE_TEST_SUITE_P.
// Do not also compile that suite separately into the same test executable.
#include <gtest/gtest.h>
#include "chronolog/clock.h"
namespace chronolog::contract
{
// Factory provides deterministic chrony/kernel inputs; stepPhysical changes only
// realtime, not the HLC state. Source values: delay=20, dispersion=3, driftAge=2.
struct ClockHarness
{
    std::unique_ptr<Clock> sut;
    std::function<void(int64_t)> stepPhysical;
    std::function<void(ClockStatus)> setStatus;
};
using ClockFactory = std::function<std::unique_ptr<ClockHarness>()>;
class ClockContract: public ::testing::TestWithParam<ClockFactory>
{
protected:
    std::unique_ptr<ClockHarness> h;
    void SetUp() override
    {
        h = GetParam()();
        ASSERT_NE(h, nullptr);
        ASSERT_NE(h->sut, nullptr);
        ASSERT_TRUE(h->stepPhysical);
        ASSERT_TRUE(h->setStatus);
        h->setStatus(ClockStatus::Synced);
        h->stepPhysical(100);
    }
};

TEST_P(ClockContract, PhysicalReadingAndConservativeBound)
{
    auto n = h->sut->now();
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n->physical_ns, 100);
    EXPECT_EQ(n->status, ClockStatus::Synced);
    ASSERT_TRUE(n->uncertainty_ns);
    EXPECT_GE(*n->uncertainty_ns, 15u);
    auto u = h->sut->uncertainty();
    ASSERT_TRUE(u.ok());
    EXPECT_EQ(*u, n->uncertainty_ns);
}

TEST_P(ClockContract, HlcMonotonicUnderBackwardPhysicalStep)
{
    auto a = h->sut->tick();
    h->stepPhysical(50);
    auto b = h->sut->tick();
    EXPECT_GT(b, a);
}

TEST_P(ClockContract, ObservePreservesReadThenWriteCausality)
{
    Hlc remote{200, 7};
    auto a = h->sut->observe(remote);
    EXPECT_GT(a, remote);
    auto b = h->sut->tick();
    EXPECT_GT(b, a);
}

TEST_P(ClockContract, StandardHlcLogicalTick)
{
    auto a = h->sut->tick();
    auto b = h->sut->tick();
    EXPECT_EQ(b.physical_ns, a.physical_ns);
    EXPECT_EQ(b.logical, a.logical + 1);
}

TEST_P(ClockContract, UnsyncedAndUnavailableHaveNoFiniteBound)
{
    for(auto status: {ClockStatus::Unsynced, ClockStatus::Unavailable})
    {
        h->setStatus(status);
        auto n = h->sut->now();
        ASSERT_TRUE(n.ok());
        EXPECT_EQ(n->status, status);
        EXPECT_FALSE(n->uncertainty_ns);
        auto u = h->sut->uncertainty();
        ASSERT_TRUE(u.ok());
        EXPECT_FALSE(*u);
    }
}

TEST_P(ClockContract, UnavailableSourceKeepsHlcAdvancing) {
 auto before=h->sut->tick(); h->setStatus(ClockStatus::Unavailable);
 auto reading=h->sut->now(); ASSERT_TRUE(reading.ok()); EXPECT_EQ(reading->status,ClockStatus::Unavailable); EXPECT_FALSE(reading->uncertainty_ns);
 auto next=h->sut->tick(); EXPECT_GT(next,before); auto merged=h->sut->observe(Hlc{200,7}); EXPECT_GT(merged,(Hlc{200,7})); EXPECT_GT(merged,next);
}
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(ClockContract);
} // namespace chronolog::contract
