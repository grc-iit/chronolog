#pragma once
#include <limits>
#include <algorithm>
#include "chronolog/types.h"
namespace chronolog::player
{
inline Range physicalWindow(const Range& range, bool policy)
{
    const auto low = std::numeric_limits<int64_t>::min();
    const auto high = std::numeric_limits<int64_t>::max();
    if(!policy)
        return {Range::Axis::Hlc, {low, 0}, {high, UINT32_MAX}};
    const PhysicalPolicy constants;
    auto clamp = [&](const __int128 value)
    { return static_cast<int64_t>(std::clamp(value, static_cast<__int128>(low), static_cast<__int128>(high))); };
    const __int128 start =
            static_cast<__int128>(range.start.physical_ns) - constants.skew_limit_ns - constants.hlc_lead_ns;
    const __int128 end =
            static_cast<__int128>(range.end.physical_ns) + constants.acceptance_window_ns + constants.hlc_lead_ns;
    return {Range::Axis::Hlc, {clamp(start), 0}, {clamp(end), end >= high ? UINT32_MAX : 0}};
}
inline bool physicalBounded(const Event& e)
{
    return e.physical.status == ClockStatus::Synced && e.physical.uncertainty_ns &&
           *e.physical.uncertainty_ns <= PhysicalPolicy{}.uncertainty_cap_ns;
}
} // namespace chronolog::player
