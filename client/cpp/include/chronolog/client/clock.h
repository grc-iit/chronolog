#pragma once
#include <functional>
#include <memory>
#include "chronolog/types.h"
struct timex;

namespace chronolog::client
{
using NtpAdjtime = std::function<int(timex*)>;
using TimeSource = std::function<TimeReading()>;
class ChronoClock
{
public:
    explicit ChronoClock(TimeSource source = {});
    ChronoClock(TimeSource source, NtpAdjtime query);
    ~ChronoClock();
    ChronoClock(const ChronoClock&) = delete;
    ChronoClock& operator=(const ChronoClock&) = delete;
    absl::StatusOr<TimeReading> now();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chronolog::client
