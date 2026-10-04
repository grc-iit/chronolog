#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "chronolog/clock.h"
#include "common/clock/HlcCore.h"
#include "common/clock/CeilingControl.h"

namespace chronolog
{

// Inputs of SystemClock. A missing realtime_ns means the physical source is lost.
struct SystemClockSource
{
    std::function<TimeReading()> snapshot;
    std::function<std::optional<int64_t>()> realtime_ns;
    std::function<ClockStatus()> status;
    std::function<std::optional<uint64_t>()> uncertainty_ns;

    // CLOCK_REALTIME, status Unsynced, no bound. Chrony parsing is not wired yet.
    static SystemClockSource kernel();
};

class SystemClock
    : public Clock
    , public CeilingControl
{
public:
    SystemClock();
    explicit SystemClock(SystemClockSource source);

    absl::StatusOr<TimeReading> now() const override;
    Hlc tick() override;
    Hlc observe(Hlc remote) override;
    absl::StatusOr<std::optional<uint64_t>> uncertainty() const override;

    absl::StatusOr<CheckedAssignment> assignChecked(Hlc floor, PhysicalInterval interval) override
    {
        auto reading = sample();
        return hlc_.assignChecked(reading.physical_ns, floor, interval);
    }
    void setCeiling(Hlc ceiling) override { hlc_.setCeiling(ceiling); }
    void observeFloor(Hlc floor) override { hlc_.observeFloor(floor); }
    int64_t acceptanceClock() override
    {
        auto reading = sample();
        return hlc_.acceptanceClock(reading.physical_ns);
    }
    void raiseAcceptanceClock(int64_t floor) override { hlc_.raiseAcceptanceClock(floor); }

private:
    TimeReading sample() const;

    SystemClockSource source_;
    HlcCore hlc_;
};

} // namespace chronolog
