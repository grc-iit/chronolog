#include "chronolog/client/clock.h"
#include <chrono>
#include <limits>
#include <mutex>
namespace chronolog::client
{
struct ChronoClock::Impl
{
    TimeSource source;
    std::mutex mutex;
    std::optional<int64_t> last;
};
ChronoClock::ChronoClock(TimeSource source)
    : impl_(std::make_unique<Impl>())
{
    impl_->source = std::move(source);
}
ChronoClock::~ChronoClock() = default;
absl::StatusOr<TimeReading> ChronoClock::now()
{
    std::lock_guard lock(impl_->mutex);
    TimeReading reading;
    if(impl_->source)
    {
        try
        {
            reading = impl_->source();
        }
        catch(...)
        {
            return absl::UnavailableError("time source failed");
        }
    }
    else
    {
        reading.physical_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
        reading.status = ClockStatus::Unsynced;
    }
    if(reading.status == ClockStatus::Synced && !reading.uncertainty_ns)
        return absl::InvalidArgumentError("synced time source requires uncertainty");
    if(reading.status != ClockStatus::Synced)
        reading.uncertainty_ns.reset();
    if(impl_->last && reading.physical_ns <= *impl_->last)
    {
        if(*impl_->last == std::numeric_limits<int64_t>::max())
            return absl::OutOfRangeError("physical clock exhausted");
        reading.physical_ns = *impl_->last + 1;
        // Clamping invalidates the source's bound on this adjusted reading.
        reading.status = ClockStatus::Unsynced;
        reading.uncertainty_ns.reset();
    }
    impl_->last = reading.physical_ns;
    return reading;
}
} // namespace chronolog::client
