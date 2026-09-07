#ifndef KEEPER_TAIL_READER_H
#define KEEPER_TAIL_READER_H

#include <cstddef>
#include <vector>

#include <chronolog_types.h>
#include <chronolog_client.h>

namespace chronolog
{

class KeeperRecordingClient;

// Tail read: two-phase scatter/gather across a story's assigned keepers.
// Phase 1 collects each keeper's last-n EventSequences (remembering which keeper
// reported each), then selects the global last-n. Phase 2 fetches only those
// payloads from the keepers that hold them. Fills `events` ascending by
// EventSequence (duplicates across keepers collapse).
//
// A tail read is best-effort across the recording group: it reports the latest
// events, with no completeness promise. One keeper failing therefore degrades the
// result (its events are missing) but is still a successful read, logged at WARNING.
// EVERY keeper failing is not a read at all and must not look like an empty tail --
// callers poll on an empty result waiting for events to become visible, so reporting
// success there would turn an outage into an apparently idle story.
//
// Returns:
//   CL_ERR_NO_KEEPERS      - `keepers` is empty
//   CL_ERR_QUERY_TIMED_OUT - no keeper answered, and at least one timed out
//   CL_ERR_UNKNOWN         - no keeper answered, all with non-timeout RPC errors
//   CL_ERR_PARTIAL_RESULT  - every keeper answered but fewer payloads came back
//                            than phase 1 promised: events were released from a
//                            keeper tail between the two phases. `events` holds
//                            what did arrive, so the result is usable but short.
//   CL_SUCCESS             - the read is complete; `events` holds what was
//                            retrieved, ascending by EventSequence (duplicates across
//                            keepers collapse). Zero events means the story genuinely
//                            has no sealed tail yet, or n == 0.
int gather_story_tail(std::vector<KeeperRecordingClient*> const& keepers,
                      StoryId const& story_id,
                      std::size_t n,
                      std::vector<Event>& events);

} // namespace chronolog

#endif
