#pragma once
#include <functional>
#include <memory>
#include "chronolog/types.h"
namespace chronolog::client
{
using TimeSource = std::function<TimeReading()>;
class ChronoClock
{
public:
    explicit ChronoClock(TimeSource source = {});
    ~ChronoClock();
    ChronoClock(const ChronoClock&) = delete;
    ChronoClock& operator=(const ChronoClock&) = delete;
    absl::StatusOr<TimeReading> now();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chronolog::client
