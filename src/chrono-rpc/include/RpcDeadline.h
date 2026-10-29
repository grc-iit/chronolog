#ifndef CHRONOLOG_RPC_DEADLINE_H
#define CHRONOLOG_RPC_DEADLINE_H

#include <chrono>
#include <optional>
#include <stdexcept>

namespace chronolog::rpc
{
namespace detail
{
struct RpcDeadlineTestAccess;
}

class RpcDeadline
{
public:
    using Clock = std::chrono::steady_clock;

    static RpcDeadline after(std::chrono::milliseconds budget) { return fromStart(Clock::now(), budget); }

    // Capture started at the public entry, before policy/admission/lookup work.
    static RpcDeadline fromStart(Clock::time_point started, std::chrono::milliseconds budget)
    {
        constexpr auto maxBudget = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
        if(budget <= std::chrono::milliseconds::zero() || budget > maxBudget)
            throw std::invalid_argument("Invalid RPC deadline budget");
        auto duration = std::chrono::duration_cast<Clock::duration>(budget);
        auto latestStart = Clock::time_point::max() - duration;
        return RpcDeadline(started > latestStart ? Clock::time_point::max() : started + duration);
    }

    Clock::time_point expiresAt() const noexcept { return expires_; }

    Clock::duration remaining() const noexcept
    {
        auto now = now_();
        if(now >= expires_)
            return Clock::duration::zero();
        auto end = expires_.time_since_epoch().count();
        auto begin = now.time_since_epoch().count();
        if(begin < 0 && end > Clock::duration::max().count() + begin)
            return Clock::duration::max();
        return expires_ - now;
    }

    bool expired() const noexcept { return now_() >= expires_; }

    std::optional<std::chrono::milliseconds> cappedAttempt(std::chrono::milliseconds cap) const noexcept
    {
        if(cap <= std::chrono::milliseconds::zero())
            return std::nullopt;
        auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(remaining());
        if(whole <= std::chrono::milliseconds::zero())
            return std::nullopt;
        return whole < cap ? whole : cap;
    }

private:
    friend struct detail::RpcDeadlineTestAccess;
    explicit RpcDeadline(Clock::time_point expires) noexcept
        : expires_(expires)
    {}
    Clock::time_point expires_;
    Clock::time_point (*now_)() noexcept = Clock::now;
};
} // namespace chronolog::rpc
#endif
