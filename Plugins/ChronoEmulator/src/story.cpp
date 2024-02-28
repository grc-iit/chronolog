#include <string>
#include <chrono>
#include "story.h"

/**
 * Records an event with the current system timestamp.
 */
uint64_t StoryHandle::record(const std::string &event)
{
    if(!acquired)
    {
        return 1;
    }
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast <std::chrono::milliseconds>(duration).count();

    events.emplace_back(static_cast<uint64_t>(millis), event);

    return static_cast<uint64_t>(millis);
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
    std::vector <std::string> matchingEvents;

    for(const auto &[eventTimestamp, event]: events)
    {
        if(eventTimestamp == timestamp)
        {
            matchingEvents.push_back(event);
        }
    }

    return matchingEvents;
}