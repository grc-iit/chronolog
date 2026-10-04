#include <gtest/gtest.h>
#include <limits>
#include "chronolog/client/clock.h"
#include "common/clock/KernelClock.h"
#include "common/clock/tests/ntp_cases.h"
using namespace chronolog;
using namespace chronolog::client;
TEST(ClientClock, StrictlyIncreasesEvenWhenRawStepsBackward)
{
    int64_t raw = 0;
    ChronoClock clock([&] { return TimeReading{raw, {}, ClockStatus::Unsynced}; });
    int64_t previous = 0;
    for(auto value: {100, 101, 500, 200, 201, 202})
    {
        raw = value;
        auto reading = clock.now();
        ASSERT_TRUE(reading.ok());
        EXPECT_GT(reading->physical_ns, previous);
        EXPECT_EQ(reading->status, ClockStatus::Unsynced);
        EXPECT_FALSE(reading->uncertainty_ns);
        previous = reading->physical_ns;
    }
}
TEST(ClientClock, SourceBoundsSurviveOnlyUnclampedReadings)
{
    ChronoClock clock([] { return TimeReading{100, 5, ClockStatus::Synced}; });
    auto first = clock.now();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->uncertainty_ns, 5);
    auto second = clock.now();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second->physical_ns, 101);
    EXPECT_EQ(second->status, ClockStatus::Unsynced);
    EXPECT_FALSE(second->uncertainty_ns);
    ChronoClock saturated([] { return TimeReading{std::numeric_limits<int64_t>::max(), {}, ClockStatus::Unsynced}; });
    ASSERT_TRUE(saturated.now().ok());
    EXPECT_EQ(saturated.now().status().code(), absl::StatusCode::kOutOfRange);
}

TEST(ClientClockNtp, MatchesKernelMappingWithoutOverflow)
{
    for(const auto& test: clock_test::cases)
    {
        SCOPED_TRACE(test.name);
        int calls = 0;
        auto query = [&](timex* value)
        {
            EXPECT_EQ(value->modes, 0u);
            ++calls;
            value->status = test.flags;
            value->maxerror = test.maxerror_us;
            return test.result;
        };
        ChronoClock clock({}, query);
        const auto reading = clock.now();
        ASSERT_TRUE(reading.ok()) << reading.status();
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(reading->status, test.status);
        EXPECT_EQ(reading->uncertainty_ns, test.bound_ns);
        EXPECT_EQ(reading->status == ClockStatus::Synced, reading->uncertainty_ns.has_value());
        if(reading->uncertainty_ns)
        {
            EXPECT_LE(*reading->uncertainty_ns, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
        }
        const auto kernel = KernelClock::ntpState(query);
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(reading->status, kernel.status);
        EXPECT_EQ(reading->uncertainty_ns, kernel.bound);
    }
}
