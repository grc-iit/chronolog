#include "clock/SystemClock.h"

#include <time.h>

namespace chronolog
{

SystemClockSource SystemClockSource::kernel()
{
    SystemClockSource source;
    source.realtime_ns = []() -> std::optional<int64_t>
    {
        timespec ts{};
        if(clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return std::nullopt;
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    };
    source.status = [] { return ClockStatus::Unsynced; };
    source.uncertainty_ns = []() -> std::optional<uint64_t> { return std::nullopt; };
    return source;
}

SystemClock::SystemClock()
    : SystemClock(SystemClockSource::kernel())
{}

SystemClock::SystemClock(SystemClockSource source)
    : source_(std::move(source))
{}

TimeReading SystemClock::sample() const
{
    if(source_.snapshot)
        return source_.snapshot();
    TimeReading reading;
    auto realtime = source_.realtime_ns();
    reading.status = realtime ? source_.status() : ClockStatus::Unavailable;
    reading.physical_ns = realtime.value_or(0);
    if(reading.status == ClockStatus::Synced)
    {
        reading.uncertainty_ns = source_.uncertainty_ns();
        // A Synced reading must carry a finite bound.
        if(!reading.uncertainty_ns)
            reading.status = ClockStatus::Unsynced;
    }
    return reading;
}

absl::StatusOr<TimeReading> SystemClock::now() const { return sample(); }

Hlc SystemClock::tick()
{
    auto reading = sample();
    return hlc_.tick(reading.status == ClockStatus::Unavailable ? 0 : reading.physical_ns);
}

Hlc SystemClock::observe(Hlc remote)
{
    auto reading = sample();
    return hlc_.observe(reading.status == ClockStatus::Unavailable ? 0 : reading.physical_ns, remote);
}

absl::StatusOr<std::optional<uint64_t>> SystemClock::uncertainty() const { return sample().uncertainty_ns; }

} // namespace chronolog
