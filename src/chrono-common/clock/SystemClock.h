#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "chronolog/clock.h"
#include "clock/HlcCore.h"

namespace chronolog
{

// Inputs of SystemClock. A missing realtime_ns means the physical source is lost.
struct SystemClockSource
{
    std::function<std::optional<int64_t>()> realtime_ns;
    std::function<ClockStatus()> status;
    std::function<std::optional<uint64_t>()> uncertainty_ns;

    // CLOCK_REALTIME, status Unsynced, no bound. Chrony parsing is not wired yet.
    static SystemClockSource kernel();
};

class SystemClock final: public Clock
{
public:
    SystemClock();
    explicit SystemClock(SystemClockSource source);

    absl::StatusOr<TimeReading> now() const override;
    Hlc tick() override;
    Hlc observe(Hlc remote) override;
    absl::StatusOr<std::optional<uint64_t>> uncertainty() const override;

private:
    TimeReading sample() const;

    SystemClockSource source_;
    HlcCore hlc_;
};

} // namespace chronolog
