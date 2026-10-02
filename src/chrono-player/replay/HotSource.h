#pragma once

#include <limits>
#include <map>
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
    // The Keeper's instance as its trailer reported it, empty when it did not answer.
    std::string instance{};
    // The FetchHot status; a FAILED_PRECONDITION names a destroyed story or a Route or instance change.
    absl::StatusCode status{absl::StatusCode::kOk};
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

inline Hlc maxHlc() { return {std::numeric_limits<int64_t>::max(), std::numeric_limits<uint32_t>::max()}; }

// One source of a Tail round: a Route Keeper, or a predecessor by the epoch it owned.
struct SourceId
{
    std::string process_id;
    Epoch predecessor_epoch{};
    auto operator<=>(const SourceId&) const = default;
};
// Where each source's next reply must start; a source not listed starts at the Tail's frontier.
struct TailStarts: std::map<SourceId, Hlc>
{
    // Full buffers supply their final coverage without another FetchHot in the same epoch and instance.
    std::map<SourceId, KeeperFrontier> retained;
};

class HotSource
{
public:
    virtual ~HotSource() = default;
    // Whole-query failures (NOT_FOUND, UNAVAILABLE) are statuses. Per-Keeper failures are
    // answered=false entries. Sources may return events outside range; consumers filter.
    virtual absl::StatusOr<HotFetch> fetch(StoryId story, const Range& range) const = 0;
    virtual absl::StatusOr<HotFetch> fetchPhysical(StoryId story, const Range& range, bool) const
    {
        return fetch(story, range);
    }
    // A Tail round (I6.13): every source answers from its own lower bound in `starts`, which is never below `from`,
    // with a budget of its own. The default is only for untruncated finite test sources; it cannot
    // make progress through a truncated suffix or suspend a full buffer. Production overrides it.
    virtual absl::StatusOr<HotFetch> fetchTail(StoryId story, Hlc from, const TailStarts&) const
    {
        return fetch(story, Range{Range::Axis::Hlc, from, maxHlc()});
    }
};

} // namespace chronolog::player
