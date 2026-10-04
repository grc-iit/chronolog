#pragma once
#include "chronolog/types.h"
namespace chronolog
{
inline absl::StatusOr<PhysicalInterval> physicalInterval(TimeReading reading, PhysicalPolicy policy = {})
{
    const bool bounded = reading.status == ClockStatus::Synced && reading.uncertainty_ns &&
                         *reading.uncertainty_ns <= policy.uncertainty_cap_ns;
    const auto u = bounded ? static_cast<int64_t>(*reading.uncertainty_ns) : 0;
    if(reading.physical_ns < INT64_MIN + u || reading.physical_ns > INT64_MAX - u)
        return absl::InvalidArgumentError("unrepresentable physical interval");
    return PhysicalInterval{reading.physical_ns - u, reading.physical_ns + u, bounded};
}
} // namespace chronolog
