#include "chronicle.h"
#include <algorithm>

namespace chronoemulator
{

Chronicle::Chronicle(const std::string &name, const std::unordered_map <std::string, std::string> &attributes): name(
        name), attributes(attributes)
{}

int Chronicle::addStory(const std::string &storyName)
{
    auto it = stories.find(storyName);
    if(it == stories.end())
    {
        // If a story with the same name does not exist, create and add the new story
        std::shared_ptr <StoryHandle> newStory = std::make_shared <StoryHandle>(storyName);
        stories.emplace(storyName, newStory);
        return 0; // Success
    }
    else
    {
        return -1; // A story with the same name already exists, error
    }

}

int Chronicle::deleteStory(const std::string &storyName)
{
    // Find the story handle by the story name
    auto it = stories.find(storyName);
    if (it != stories.end())
    {
        // Check if the story is acquired
        if (it->second->acquired)
        {
            return -1; // Story is acquired and cannot be deleted
        }

        // Erase the story
        stories.erase(it);
        return 0; // Story found and deleted
    }
    return -1; // Story not found
}

bool Chronicle::storyExists(const std::string &story_name) const
{
    return stories.find(story_name) != stories.end();
}

StoryHandle*Chronicle::acquireStory(const std::string &story_name)
{
    auto it = stories.find(story_name);
    if(it != stories.end())
    {
        if(!it->second->acquired)
        {
            it->second->acquired = true;
        }
        return it->second.get();
    }
    return nullptr;
}


int Chronicle::releaseStory(const std::string &story_name)
{
    auto it = stories.find(story_name);
    if (it != stories.end())
    {
        it->second->acquired = false;
        return 0;
    }
    return -1;
}
}