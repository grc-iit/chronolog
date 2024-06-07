#include <string>
#include <chrono>
#include <thread>
#include "story.h"

namespace chronoemulator
{
/**
 * Records an event with the current system timestamp.
 */
uint64_t StoryHandle::record(const std::string &event)
{
    if (!acquired)
    {
        return 1;  // Indicates the story was not acquired for recording
    }
    // Get the current high-resolution timestamp
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    uint64_t timestamp = static_cast<uint64_t>(nanos);

    // Insert the event into the unordered_map at the calculated timestamp.
    events[timestamp].push_back(event);
    return timestamp; // Return the timestamp at which the event was recorded
}

/**
 * Replays events that match the specified timestamp.
 */
std::vector <std::string> StoryHandle::replay(uint64_t timestamp)
{
    if(!acquired)
    {
        return {};
    }
    // Use the timestamp to directly access the vector of events.
    auto it = events.find(timestamp);
    if(it != events.end())
    {
        return it->second;
    }
    else
    {
        return {}; // Return an empty vector if no events are found for the timestamp.
    }
}
}