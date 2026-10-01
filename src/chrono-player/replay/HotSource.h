#pragma once

#include <string>
#include <vector>
#include "chronolog/types.h"

namespace chronolog::player
{

// One Keeper's answer to a hot fetch. `sealed` is the Keeper's exclusive frontier F.
struct KeeperFrontier
{
    std::string process_id;
    Epoch epoch{};
    Hlc sealed;
    bool answered{true};
    bool truncated{};
    Hlc evicted_below{};
    Epoch expected_epoch{};
    bool predecessor{};
    Hlc own_cut{};
    std::optional<Hlc> truncated_at{};
    std::optional<int64_t> physical_frontier{};
};

struct KeeperFetch
{
    KeeperFrontier frontier;
    std::vector<Event> events;
};

// Acquisition view entry, used only to name laggards.
struct WriterAssignment
{
    uint64_t writer_id{};
    uint64_t incarnation{};
    std::string keeper;
};

struct HotFetch
{
    Epoch route_epoch{};
    // One entry per Keeper in the story Route; an unreachable Keeper has answered=false.
    std::vector<KeeperFetch> keepers;
    std::vector<WriterAssignment> writers;
    // The source will produce no more events for this story; tails end orderly.
    bool closed{};
    Hlc archived_below{};
    std::vector<Range> abandoned{};
    bool physical_policy{};
};

class HotSource
{
public:
    virtual ~HotSource() = default;
    // Whole-query failures (NOT_FOUND, UNAVAILABLE) are statuses. Per-Keeper failures are
    // answered=false entries. Sources may return events outside range; consumers filter.
    virtual absl::StatusOr<HotFetch> fetch(StoryId story, const Range& range) const = 0;
};

} // namespace chronolog::player
