#include "ChronologTimer.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <client_errcode.h>

chronolog::ChronologTimer::ChronologTimer(RawSource source)
    : source_(source ? std::move(source) : RawSource(unixNanoseconds))
{}

chronolog::TimestampResult chronolog::ChronologTimer::unixNanoseconds()
{
    auto raw = std::chrono::system_clock::now().time_since_epoch();
    if(raw <= decltype(raw)::zero())
        return {CL_ERR_CLOCK_UNAVAILABLE, 0};
    // A wide representation makes the explicit conversion checked before
    // narrowing, including platforms whose system clock has another period.
    using WideNanoseconds = std::chrono::duration<unsigned __int128, std::nano>;
    auto nanoseconds = std::chrono::duration_cast<WideNanoseconds>(raw).count();
    if(nanoseconds == 0 || nanoseconds > std::numeric_limits<uint64_t>::max())
        return {CL_ERR_CLOCK_UNAVAILABLE, 0};
    return {CL_SUCCESS, static_cast<uint64_t>(nanoseconds)};
}

chronolog::TimestampResult chronolog::ChronologTimer::next(std::chrono::steady_clock::time_point deadline) noexcept
{
    using Clock = std::chrono::steady_clock;
    try
    {
        if(Clock::now() >= deadline)
            return {CL_ERR_DEADLINE_EXCEEDED, 0};
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        if(!lock.try_lock_until(deadline) || Clock::now() >= deadline)
            return {CL_ERR_DEADLINE_EXCEEDED, 0};
        if(previous_ == std::numeric_limits<uint64_t>::max())
            return {CL_ERR_TIMESTAMP_EXHAUSTED, 0};
        auto raw = source_();
        if(Clock::now() >= deadline)
            return {CL_ERR_DEADLINE_EXCEEDED, 0};
        if(raw.status != CL_SUCCESS || raw.timestamp == 0)
            return {CL_ERR_CLOCK_UNAVAILABLE, 0};
        previous_ = std::max(raw.timestamp, previous_ + 1);
        return {CL_SUCCESS, previous_};
    }
    catch(...)
    {
        return {CL_ERR_CLOCK_UNAVAILABLE, 0};
    }
}
