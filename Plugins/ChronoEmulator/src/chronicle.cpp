#include "chronicle.h"
#include <algorithm>

Chronicle::Chronicle(const std::string &name, const std::unordered_map <std::string, std::string> &attributes): name(
        name), attributes(attributes)
{}

int Chronicle::addStory(const std::string &storyName)
{
    // Check if a story with the same name already exists
    auto it = std::find_if(stories.begin(), stories.end(), [&storyName](
            const std::shared_ptr <StoryHandle> &existingStory)
    {
        return existingStory->storyName == storyName;
    });

    // If a story with the same name does not exist, create and add the new story
    if(it == stories.end())
    {
        std::shared_ptr <StoryHandle> newStory = std::make_shared <StoryHandle>(storyName);
        stories.push_back(newStory);
        return 0; // Success
    }
    else
    {
        // A story with the same name already exists, error
        return -1; // Error
    }
}

int Chronicle::deleteStory(const std::string &storyName)
{
    auto it = std::remove_if(stories.begin(), stories.end(), [storyName](const std::shared_ptr <StoryHandle> &story)
    {
        return story->storyName == storyName; // Compare against storyName instead of getId()
    });

    if(it != stories.end())
    {
        stories.erase(it, stories.end());
        it->reset();
        return 0; // Story found and deleted
    }

    return -1; // Story not found
}

bool Chronicle::storyExists(const std::string &story_name) const
{
    auto it = std::find_if(stories.begin(), stories.end(), [&story_name](const std::shared_ptr <StoryHandle> &story)
    {
        return story->storyName == story_name;
    });

    return it != stories.end();
}

StoryHandle*Chronicle::acquireStory(const std::string &story_name)
{
    auto it = std::find_if(stories.begin(), stories.end(), [&story_name](const std::shared_ptr <StoryHandle> &story)
    {
        return story->storyName == story_name;
    });

    if(it != stories.end() && !(*it)->acquired)
    {
        (*it)->acquired = true;
        return it->get();
    }
    return nullptr;
}

int Chronicle::releaseStory(const std::string &story_name)
{
    auto it = std::find_if(stories.begin(), stories.end(), [&story_name](const std::shared_ptr <StoryHandle> &story)
    {
        return story->storyName == story_name;
    });

    if(it != stories.end() && (*it)->acquired)
    {
        (*it)->acquired = false;
        return 0;
    }
    return -1;
}
