#include "clock/KernelClock.h"
#include <sys/timex.h>

namespace chronolog
{
SystemClockSource KernelClock::source(const std::shared_ptr<State>& state)
{
    auto result = SystemClockSource::kernel();
    result.snapshot = [state, realtime = result.realtime_ns]
    {
        const auto physical = realtime();
        std::lock_guard lock(state->mutex);
        return TimeReading{physical.value_or(0),
                           physical ? state->bound : std::nullopt,
                           physical ? state->status : ClockStatus::Unavailable};
    };
    return result;
}
KernelClock::KernelClock()
    : KernelClock(std::make_shared<State>())
{}
KernelClock::KernelClock(std::shared_ptr<State> state)
    : SystemClock(source(state))
    , state_(std::move(state))
{
    refresh_ = std::thread(
            [state = state_]
            {
                for(;;)
                {
                    timex value{};
                    const int result = ntp_adjtime(&value);
                    std::unique_lock lock(state->mutex);
                    state->bound.reset();
                    state->status = result < 0 ? ClockStatus::Unavailable : ClockStatus::Unsynced;
                    if(result >= 0 && !(value.status & STA_UNSYNC) && value.maxerror >= 0 &&
                       static_cast<uint64_t>(value.maxerror) <= PhysicalPolicy{}.uncertainty_cap_ns / 1000)
                    {
                        state->status = ClockStatus::Synced;
                        state->bound = static_cast<uint64_t>(value.maxerror) * 1000;
                    }
                    if(state->cv.wait_for(lock, std::chrono::seconds(1), [&] { return state->stopped; }))
                        return;
                }
            });
}
KernelClock::~KernelClock()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
    }
    state_->cv.notify_all();
    refresh_.join();
}
} // namespace chronolog
