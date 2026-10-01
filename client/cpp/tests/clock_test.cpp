#include <gtest/gtest.h>
#include <limits>
#include "chronolog/client/clock.h"
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
