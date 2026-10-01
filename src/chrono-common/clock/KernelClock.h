#pragma once

#include <condition_variable>
#include <thread>
#include "clock/SystemClock.h"

namespace chronolog
{
class KernelClock final: public SystemClock
{
public:
    KernelClock();
    ~KernelClock() override;

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable cv;
        ClockStatus status{ClockStatus::Unavailable};
        std::optional<uint64_t> bound;
        bool stopped{};
    };
    explicit KernelClock(std::shared_ptr<State> state);
    static SystemClockSource source(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
    std::thread refresh_;
};
} // namespace chronolog
