#pragma once

#include <mutex>

#include "chronolog/clock.h"
#include "clock/HlcCore.h"
#include "clock/CeilingControl.h"

namespace chronolog
{

// Deterministic Clock for tests. Physical time and status are set by the test.
class FakeClock final
    : public Clock
    , public CeilingControl
{
public:
    explicit FakeClock(int64_t physical_ns = 0, uint64_t synced_bound_ns = 15)
        : physical_ns_(physical_ns)
        , synced_bound_ns_(synced_bound_ns)
    {}

    void setPhysical(int64_t physical_ns)
    {
        std::lock_guard lock(mutex_);
        physical_ns_ = physical_ns;
    }

    void setStatus(ClockStatus status)
    {
        std::lock_guard lock(mutex_);
        status_ = status;
    }

    absl::StatusOr<TimeReading> now() const override { return sample(); }

    Hlc tick() override
    {
        auto reading = sample();
        return hlc_.tick(reading.status == ClockStatus::Unavailable ? 0 : reading.physical_ns);
    }

    Hlc observe(Hlc remote) override
    {
        auto reading = sample();
        return hlc_.observe(reading.status == ClockStatus::Unavailable ? 0 : reading.physical_ns, remote);
    }

    absl::StatusOr<std::optional<uint64_t>> uncertainty() const override { return sample().uncertainty_ns; }

    absl::StatusOr<CheckedAssignment> assignChecked(Hlc floor, PhysicalInterval interval) override
    {
        auto reading = sample();
        return hlc_.assignChecked(reading.status == ClockStatus::Unavailable ? INT64_MIN : reading.physical_ns,
                                  floor,
                                  interval);
    }
    void setCeiling(Hlc ceiling) override { hlc_.setCeiling(ceiling); }
    void observeFloor(Hlc floor) override { hlc_.observeFloor(floor); }
    int64_t acceptanceClock() override
    {
        auto reading = sample();
        return hlc_.acceptanceClock(reading.status == ClockStatus::Unavailable ? INT64_MIN : reading.physical_ns);
    }
    void raiseAcceptanceClock(int64_t floor) override { hlc_.raiseAcceptanceClock(floor); }

private:
    TimeReading sample() const
    {
        std::lock_guard lock(mutex_);
        TimeReading reading;
        reading.physical_ns = physical_ns_;
        reading.status = status_;
        if(status_ == ClockStatus::Synced)
            reading.uncertainty_ns = synced_bound_ns_;
        return reading;
    }

    mutable std::mutex mutex_;
    int64_t physical_ns_;
    uint64_t synced_bound_ns_;
    ClockStatus status_{ClockStatus::Unsynced};
    HlcCore hlc_;
};

} // namespace chronolog
