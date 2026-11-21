#ifndef CHRONOLOG_TIMER_H
#define CHRONOLOG_TIMER_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace chronolog
{
struct TimestampResult
{
    int status;
    uint64_t timestamp;
};

// One client-implementation lifetime owns the floor across all stories and
// reconnects. This orders local assignments, not processes or client restarts.
// After a wall-clock rollback assignments can lead wall time without a lead cap.
class ChronologTimer
{
public:
    // Injected sources report Unix nanoseconds and must return promptly. A
    // non-success status, zero reading or exception means clock unavailable.
    using RawSource = std::function<TimestampResult()>;
    explicit ChronologTimer(RawSource source = {});
    TimestampResult next(std::chrono::steady_clock::time_point deadline) noexcept;

private:
    static TimestampResult unixNanoseconds();
    RawSource source_;
    std::timed_mutex mutex_;
    uint64_t previous_ = 0;
};
} // namespace chronolog
#endif
