#ifndef ACTIVE_TAIL_SOURCE_H
#define ACTIVE_TAIL_SOURCE_H

#include <cstddef>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

// Interface a live story pipeline exposes so the KeeperTailStore can serve the
// most-recent events straight from the active (unsealed) timeline, in addition
// to the sealed-chunk tail. Implemented by KeeperStoryPipeline; registered with
// the KeeperTailStore while the pipeline is alive. Kept as a narrow interface so
// KeeperTailStore does not have to depend on the full pipeline definition (which
// itself depends on KeeperTailStore).
//
// Both methods are expected to synchronize against the pipeline's own timeline
// mutex so they are safe to call concurrently with background ingestion/decay.
class ActiveTailSource
{
public:
    virtual ~ActiveTailSource() = default;

    // Up to `n` most-recent EventSequences currently held in the active timeline
    // (open/unsealed chunks), returned in ascending order.
    virtual std::vector<EventSequence> activeTailSequences(std::size_t n) = 0;

    // If `seq` is present in the active timeline, copy its event into `out` and
    // return true; otherwise return false. The payload is copied under lock so
    // the returned event stays valid regardless of subsequent seal/decay.
    virtual bool findActiveEvent(EventSequence const& seq, LogEvent& out) = 0;
};

} // namespace chronolog

#endif
