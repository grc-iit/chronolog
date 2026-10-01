#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

#include "chronolog/types.h"

namespace chronolog
{

// Standard hybrid logical clock. Physical time is supplied by the caller so one core serves every Clock.
class HlcCore
{
public:
    Hlc tick(int64_t physical_ns)
    {
        std::lock_guard lock(mutex_);
        return advance(physical_ns, Hlc{});
    }

    Hlc observe(int64_t physical_ns, Hlc remote)
    {
        std::lock_guard lock(mutex_);
        return advance(physical_ns, remote);
    }

    absl::StatusOr<CheckedAssignment> assignChecked(int64_t physical, Hlc floor, PhysicalInterval interval)
    {
        std::lock_guard lock(mutex_);
        acceptance_ = std::max(acceptance_, physical);
        const auto maximum = std::max({acceptance_, floor.physical_ns, last_.physical_ns});
        if(maximum == INT64_MAX && ((floor.physical_ns == maximum && floor.logical == UINT32_MAX) ||
                                    (last_.physical_ns == maximum && last_.logical == UINT32_MAX)))
            return absl::InvalidArgumentError("HLC overflow");
        const auto saved = last_;
        const auto candidate = advance(acceptance_, floor);
        last_ = saved;
        if(ceiling_ && candidate >= *ceiling_)
            return absl::ResourceExhaustedError("WOULD_EXCEED_CEILING");
        if(candidate.physical_ns > INT64_MIN + policy_.hlc_lead_ns)
            acceptance_ = std::max(acceptance_, candidate.physical_ns - policy_.hlc_lead_ns);
        const int64_t low = acceptance_ < INT64_MIN + policy_.acceptance_window_ns
                                    ? INT64_MIN
                                    : acceptance_ - policy_.acceptance_window_ns;
        const int64_t high =
                acceptance_ > INT64_MAX - policy_.skew_limit_ns ? INT64_MAX : acceptance_ + policy_.skew_limit_ns;
        if(interval.lo < low || interval.hi > high)
            return absl::OutOfRangeError("physical reading outside acceptance window");
        last_ = candidate;
        return CheckedAssignment{candidate, acceptance_};
    }
    void setCeiling(Hlc ceiling)
    {
        std::lock_guard lock(mutex_);
        ceiling_ = ceiling;
    }
    void observeFloor(Hlc floor)
    {
        std::lock_guard lock(mutex_);
        last_ = std::max(last_, floor);
    }
    int64_t acceptanceClock(int64_t physical)
    {
        std::lock_guard lock(mutex_);
        return acceptance_ = std::max(acceptance_, physical);
    }
    void raiseAcceptanceClock(int64_t floor)
    {
        std::lock_guard lock(mutex_);
        acceptance_ = std::max(acceptance_, floor);
    }

private:
    Hlc advance(int64_t physical_ns, const Hlc& remote)
    {
        int64_t physical = std::max({physical_ns, last_.physical_ns, remote.physical_ns});
        uint32_t logical = 0;
        if(physical == last_.physical_ns || physical == remote.physical_ns)
        {
            uint32_t base = std::max(physical == last_.physical_ns ? last_.logical : 0u,
                                     physical == remote.physical_ns ? remote.logical : 0u);
            if(base == std::numeric_limits<uint32_t>::max())
                ++physical;
            else
                logical = base + 1;
        }
        last_ = Hlc{physical, logical};
        return last_;
    }

    std::mutex mutex_;
    Hlc last_{};
    std::optional<Hlc> ceiling_;
    int64_t acceptance_{INT64_MIN};
    PhysicalPolicy policy_;
};

} // namespace chronolog
