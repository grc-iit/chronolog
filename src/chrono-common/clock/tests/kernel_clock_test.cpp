#include <gtest/gtest.h>
#include "clock/KernelClock.h"
#include "ntp_cases.h"

namespace chronolog
{
TEST(KernelClockNtp, MapsReadOnlyQueriesWithoutOverflow)
{
    for(const auto& test: clock_test::cases)
    {
        SCOPED_TRACE(test.name);
        int calls = 0;
        const auto state = KernelClock::ntpState(
                [&](timex* value)
                {
                    EXPECT_EQ(value->modes, 0u);
                    ++calls;
                    value->status = test.flags;
                    value->maxerror = test.maxerror_us;
                    return test.result;
                });
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(state.status, test.status);
        EXPECT_EQ(state.bound, test.bound_ns);
        EXPECT_EQ(state.status == ClockStatus::Synced, state.bound.has_value());
        if(state.bound)
            EXPECT_LE(*state.bound, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    }
}
} // namespace chronolog
