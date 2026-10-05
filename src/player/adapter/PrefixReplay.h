#pragma once

#include <string>
#include "player/adapter/StoryCatalog.h"
#include "player/replay/HotReplay.h"

namespace chronolog::player
{

struct PrefixOptions
{
    // I6.15: the most stories one prefix Read or Tail may resolve to.
    uint32_t max_scopes{1024};
    // I6.16: re-resolutions of the story set before a Read answers LAGGING_WRITERS.
    uint32_t resolve_retries{3};
    size_t read_max_events{262144};
    size_t batch_size{1024};
};

// Reads and Tails over the stories under a path prefix, resolved through the Catalog only (I6.15, I9.3).
class PrefixReplay
{
public:
    PrefixReplay(const HotReplay& replay, const StoryCatalog& catalog, PrefixOptions options)
        : replay_(replay)
        , catalog_(catalog)
        , options_(options)
    {}

    // Resolves the set at one Catalog revision, reads every story, merges in the order of I7.7 and certifies the
    // result under I6.15 and I6.16. RESOURCE_EXHAUSTED when the prefix resolves to more than max_scopes stories.
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    read(const std::string& prefix, Range range, size_t max_events, const EventPredicate& predicate) const;

    // Fixes the story set at the revision it resolves at; the first message of the stream names it.
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(const std::string& prefix, Event position, const EventPredicate& predicate) const;

private:
    const HotReplay& replay_;
    const StoryCatalog& catalog_;
    const PrefixOptions options_;
};

} // namespace chronolog::player
