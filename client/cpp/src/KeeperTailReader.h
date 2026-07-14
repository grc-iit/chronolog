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
// Returns CL_ERR_NO_KEEPERS if `keepers` is empty; CL_SUCCESS otherwise. An
// empty tail (no sealed events yet, or n == 0) is CL_SUCCESS with zero events.
int gather_story_tail(std::vector<KeeperRecordingClient*> const& keepers,
                      StoryId const& story_id,
                      std::size_t n,
                      std::vector<Event>& events);

} // namespace chronolog

#endif
