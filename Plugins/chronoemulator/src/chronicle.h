#ifndef CHRONOEMULATOR_CHRONICLE_H
#define CHRONOEMULATOR_CHRONICLE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "story.h"

namespace chronoemulator
{

class Chronicle
{
public:
    std::string name; ///< The name of the Chronicle.
    std::unordered_map <std::string, std::string> attributes; ///< Attributes associated with the Chronicle.
    std::unordered_map <std::string, std::shared_ptr <StoryHandle>> stories; ///< Map of stories in the Chronicle, keyed by story name.
    //std::vector <std::shared_ptr <StoryHandle>> stories; ///< Vector of stories in the Chronicle.

    /**
     * @brief Constructs a new Chronicle object.
     *
     * @param name The name of the Chronicle.
     * @param attributes A map of attributes for the Chronicle.
     */
    Chronicle(const std::string &name, const std::unordered_map <std::string, std::string> &attributes);

    /**
     * @brief Adds a story to the Chronicle.
     *
     * Adds a new StoryHandle to the Chronicle if a story with the same name does not already exist.
     *
     * @param storyName The name of the story to add.
     * @return int Returns 0 on success, -1 if a story with the same name already exists.
     */
    int addStory(const std::string &storyName);

    /**
     * @brief Deletes a story from the Chronicle.
     *
     * Removes the story with the given ID from the Chronicle, if it exists.
     *
     * @param storyId The ID of the story to delete.
     * @return int Returns 0 if the story was successfully deleted, -1 if the story was not found.
     */
    int deleteStory(const std::string &storyId);

    /**
     * @brief Checks if a story exists within the Chronicle.
     *
     * Searches for a story by name and determines if it exists in the Chronicle.
     *
     * @param story_name The name of the story to check for.
     * @return bool Returns true if the story exists, false otherwise.
     */
    bool storyExists(const std::string &story_name) const;

    /**
     * @brief Acquires a story for use.
     *
     * Marks the story with the specified name as acquired if it is not already and returns a pointer to it.
     *
     * @param story_name The name of the story to acquire.
     * @return StoryHandle* A raw pointer to the acquired story, or nullptr if the story does not exist or is already acquired.
     */
    StoryHandle*acquireStory(const std::string &story_name);

    /**
     * @brief Releases a previously acquired story.
     *
     * Marks the story with the specified name as not acquired, allowing it to be acquired again in the future.
     *
     * @param story_name The name of the story to release.
     * @return int Returns 0 if the story was successfully released, -1 if the story does not exist or was not acquired.
     */
    int releaseStory(const std::string &story_name);
};
}

#endif //CHRONOEMULATOR_CHRONICLE_H
