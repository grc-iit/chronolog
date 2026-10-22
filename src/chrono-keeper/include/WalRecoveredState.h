#ifndef CHRONOLOG_WAL_RECOVERED_STATE_H
#define CHRONOLOG_WAL_RECOVERED_STATE_H

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

// What recovery read back from the WAL directory at startup: the events still
// pending after D records are applied, and where each copy of them lies, so
// the keeper can replay them into sealed chunks and the WAL can track the
// copies until the grapher confirms them.

struct RecoveredStory
{
    StoryId wal_story_id;
    bool has_names = false;
    ChronicleName chronicle;
    StoryName story;
    std::map<EventSequence, LogEvent> events;                                   // after D handling
    std::map<EventSequence, std::vector<std::pair<uint64_t, uint64_t>>> copies; // (segment, frame bytes)
};

struct RecoveredState
{
    std::vector<uint64_t> segments;            // oldest first
    std::map<StoryId, RecoveredStory> stories; // keyed by wal_story_id
    std::optional<uint64_t> reusable_spare;    // newest segment if it holds only its H record
    uint64_t bytes_read = 0;
    uint64_t oldest_segment_age_secs = 0;
};

} // namespace chronolog

#endif
