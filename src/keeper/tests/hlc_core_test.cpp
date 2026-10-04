#include <gtest/gtest.h>

#include <limits>

#include "common/clock/HlcCore.h"
#include "common/clock/SystemClock.h"

namespace chronolog
{

TEST(HlcCore, LogicalResetsWhenPhysicalAdvances)
{
    HlcCore core;
    EXPECT_EQ(core.tick(100), (Hlc{100, 0}));
    EXPECT_EQ(core.tick(100), (Hlc{100, 1}));
    EXPECT_EQ(core.tick(101), (Hlc{101, 0}));
    EXPECT_EQ(core.tick(50), (Hlc{101, 1}));
}

TEST(HlcCore, ObserveMergesRemote)
{
    HlcCore core;
    EXPECT_EQ(core.tick(100), (Hlc{100, 0}));
    EXPECT_EQ(core.observe(100, Hlc{100, 5}), (Hlc{100, 6}));
    EXPECT_EQ(core.observe(100, Hlc{200, 7}), (Hlc{200, 8}));
    EXPECT_EQ(core.observe(100, Hlc{150, 0}), (Hlc{200, 9}));
    EXPECT_EQ(core.observe(300, Hlc{200, 99}), (Hlc{300, 0}));
}

TEST(HlcCore, LogicalOverflowCarriesIntoPhysical)
{
    HlcCore core;
    core.observe(100, Hlc{100, std::numeric_limits<uint32_t>::max() - 2});
    auto a = core.tick(100);
    EXPECT_EQ(a, (Hlc{100, std::numeric_limits<uint32_t>::max()}));
    auto b = core.tick(100);
    EXPECT_GT(b, a);
    EXPECT_EQ(b, (Hlc{101, 0}));
}

TEST(SystemClock, BoundSourceLossKeepsRealtimeAcceptance)
{
    SystemClockSource source;
    source.realtime_ns = [] { return std::optional<int64_t>(100'000'000'000); };
    source.status = [] { return ClockStatus::Unavailable; };
    source.uncertainty_ns = [] { return std::optional<uint64_t>{}; };
    SystemClock clock(std::move(source));
    EXPECT_EQ(clock.now()->status, ClockStatus::Unavailable);
    auto assignment = clock.assignChecked({}, {100'000'000'000, 100'000'000'000, false});
    ASSERT_TRUE(assignment.ok()) << assignment.status();
    EXPECT_EQ(assignment->acceptance_clock_ns, 100'000'000'000);
    EXPECT_GE(assignment->hlc.physical_ns, 100'000'000'000);
    EXPECT_GE(clock.tick().physical_ns, 100'000'000'000);
}

TEST(SystemClock, KernelSourceIsUnsyncedWithoutBound)
{
    SystemClock clock;
    auto n = clock.now();
    ASSERT_TRUE(n.ok());
    EXPECT_GT(n->physical_ns, 0);
    EXPECT_EQ(n->status, ClockStatus::Unsynced);
    EXPECT_FALSE(n->uncertainty_ns);
    auto a = clock.tick();
    EXPECT_GT(clock.tick(), a);
}

} // namespace chronolog
