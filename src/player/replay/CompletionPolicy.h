#pragma once

#include <vector>
#include "chrono-player/replay/HotSource.h"

namespace chronolog::player
{

class CompletionPolicy
{
public:
    // Precedence: SourceFailed, Truncated, PhysicalAxisUnbounded, LaggingWriters.
    // `keepers` is every Keeper in the Route; `writers` only names laggards.
    static Completion decide(const Range& range,
                             Epoch route_epoch,
                             const std::vector<KeeperFrontier>& keepers,
                             const std::vector<WriterAssignment>& writers,
                             bool archive_failed = false,
                             bool physical_policy = false,
                             bool unbounded_event = false);
};

} // namespace chronolog::player
