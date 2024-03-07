#ifndef CHRONOEMULATOR_STORY_H
#define CHRONOEMULATOR_STORY_H

#include "vector"

namespace chronoemulator
{
/**
 * @brief Represents a single story with a series of timestamped events.
 *
 * This class provides functionalities to record events happening at specific timestamps
 * and to replay events that have been recorded up until a specific timestamp.
 */
class StoryHandle
{
public:
    std::string storyName; ///< The name of the story.
    std::vector <std::pair <uint64_t, std::string>> events; ///< A list of events and their timestamps.
    bool acquired = false; ///< Flag indicating if the story is currently acquired.

    /**
     * @brief Constructs a new StoryHandle object.
     *
     * @param name The name of the story.
     */
    StoryHandle(const std::string &name): storyName(name)
    {}

    /**
     * @brief Records an event in the story.
     *
     * Adds an event with the current system timestamp to the story, if the story is acquired.
     *
     * @param event The event to record.
     * @return uint64_t The timestamp of the recorded event in milliseconds since epoch,
     *                  or 1 if the story is not acquired.
     */
    uint64_t record(const std::string &event);

    /**
     * @brief Replays events from the story that match a specific timestamp.
     *
     * Retrieves a list of events that have the exact timestamp provided.
     *
     * @param timestamp The timestamp for which to replay events.
     * @return std::vector<std::string> A vector of events that match the given timestamp.
     */
    std::vector <std::string> replay(uint64_t timestamp);
};
}
#endif //CHRONOEMULATOR_STORY_H
