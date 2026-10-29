#include <gtest/gtest.h>
#include <RpcDeadline.h>
#include <atomic>
#include <limits>

using namespace std::chrono_literals;
using chronolog::rpc::RpcDeadline;

namespace
{
std::atomic<RpcDeadline::Clock::duration::rep> tick{0};
RpcDeadline::Clock::time_point fakeNow() noexcept
{
    return RpcDeadline::Clock::time_point(RpcDeadline::Clock::duration(tick.load()));
}
} // namespace

namespace chronolog::rpc::detail
{
struct RpcDeadlineTestAccess
{
    static RpcDeadline fromStart(RpcDeadline::Clock::time_point started, std::chrono::milliseconds budget)
    {
        auto deadline = RpcDeadline::fromStart(started, budget);
        deadline.now_ = fakeNow;
        return deadline;
    }
};
} // namespace chronolog::rpc::detail

TEST(RpcDeadline, PreservesEntryTimeAndCapsEachLaterAttempt)
{
    using chronolog::rpc::detail::RpcDeadlineTestAccess;
    tick = 0;
    auto deadline = RpcDeadlineTestAccess::fromStart(RpcDeadline::Clock::time_point{}, 100ms);
    EXPECT_EQ(deadline.expiresAt().time_since_epoch(), 100ms);
    tick = std::chrono::duration_cast<RpcDeadline::Clock::duration>(35ms).count();
    EXPECT_EQ(deadline.remaining(), 65ms);
    ASSERT_TRUE(deadline.cappedAttempt(80ms));
    EXPECT_EQ(*deadline.cappedAttempt(80ms), 65ms);
    EXPECT_EQ(*deadline.cappedAttempt(20ms), 20ms);
    tick = std::chrono::duration_cast<RpcDeadline::Clock::duration>(100ms).count();
    EXPECT_TRUE(deadline.expired());
    EXPECT_EQ(deadline.remaining(), RpcDeadline::Clock::duration::zero());
    EXPECT_FALSE(deadline.cappedAttempt(5ms));
}

TEST(RpcDeadline, RefusesFractionalOrNonpositiveAttempts)
{
    using chronolog::rpc::detail::RpcDeadlineTestAccess;
    tick = 0;
    auto deadline = RpcDeadlineTestAccess::fromStart(RpcDeadline::Clock::time_point{}, 2ms);
    tick = std::chrono::duration_cast<RpcDeadline::Clock::duration>(1500us).count();
    EXPECT_FALSE(deadline.expired());
    EXPECT_FALSE(deadline.cappedAttempt(5ms));
    EXPECT_FALSE(deadline.cappedAttempt(0ms));
    EXPECT_FALSE(deadline.cappedAttempt(-1ms));
}

TEST(RpcDeadline, RejectsInvalidBudgetsWithoutConvertingOverflow)
{
    EXPECT_THROW(RpcDeadline::after(0ms), std::invalid_argument);
    EXPECT_THROW(RpcDeadline::after(-1ms), std::invalid_argument);
    EXPECT_THROW(RpcDeadline::after(std::chrono::milliseconds::max()), std::invalid_argument);
}

TEST(RpcDeadline, SaturatesAbsoluteAdditionAndRemainingDifference)
{
    using chronolog::rpc::detail::RpcDeadlineTestAccess;
    using Clock = RpcDeadline::Clock;
    auto deadline = RpcDeadlineTestAccess::fromStart(Clock::time_point::max() - 1ms, 10ms);
    EXPECT_EQ(deadline.expiresAt(), Clock::time_point::max());
    tick = Clock::duration::min().count();
    EXPECT_EQ(deadline.remaining(), Clock::duration::max());
    tick = Clock::duration::max().count();
    EXPECT_TRUE(deadline.expired());
}

TEST(RpcDeadline, AfterStartsOneNewLocalBudget)
{
    auto started = RpcDeadline::Clock::now();
    auto deadline = RpcDeadline::after(20ms);
    EXPECT_GE(deadline.expiresAt(), started + 20ms);
    EXPECT_LE(deadline.expiresAt(), RpcDeadline::Clock::now() + 20ms);
    EXPECT_GT(deadline.remaining(), RpcDeadline::Clock::duration::zero());
}
