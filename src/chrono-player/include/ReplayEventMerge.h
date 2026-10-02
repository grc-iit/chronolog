#ifndef CHRONOLOG_REPLAY_EVENT_MERGE_H
#define CHRONOLOG_REPLAY_EVENT_MERGE_H

#include <algorithm>
#include <iterator>
#include <vector>

#include <chronolog_client.h>

namespace chronolog
{

// A replay collects events from the archive and from the story's keepers, and
// the same event can arrive more than once: from a keeper and from the archive
// when the grapher has written the keeper's chunk but the keeper has not heard
// yet, or twice from the archive when a re-sent chunk was written to a second
// file. Returns one ascending series with each event once, identified by
// (time, client id, index).
inline std::vector<Event> mergeReplayEvents(std::vector<Event> archived, std::vector<Event> hot)
{
    archived.insert(archived.end(), std::make_move_iterator(hot.begin()), std::make_move_iterator(hot.end()));
    // Event orders and compares by (time, client id, index)
    std::sort(archived.begin(), archived.end());
    archived.erase(std::unique(archived.begin(), archived.end()), archived.end());
    return archived;
}

} // namespace chronolog

#endif // CHRONOLOG_REPLAY_EVENT_MERGE_H
