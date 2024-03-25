#include <algorithm>
#include "chronoemulator.h"

namespace chronoemulator
{

int ChronoEmulator::Connect()
{
    isConnected = true;
    return 0; // Simulate successful connection
}

int ChronoEmulator::Disconnect()
{
    isConnected = false;
    return 0; // Simulate successful disconnection
}

int ChronoEmulator::CreateChronicle(const std::string &chronicle_name
                                    , const std::unordered_map <std::string, std::string> &attrs, int &flags)
{
    if(!isConnected)
    {
        return -1; // Connection is not established
    }

    // Search for an existing chronicle with the same name
    for(const auto &chronicle: chronicles)
    {
        if(chronicle->name == chronicle_name)
        {
            return -1; // Chronicle already exists
        }
    }

    // If not found, create a new chronicle and add it to the vector
    std::shared_ptr <Chronicle> newChronicle = std::make_shared <Chronicle>(chronicle_name, attrs);
    chronicles.push_back(newChronicle);
    return 0; // Success
}

int ChronoEmulator::DestroyChronicle(const std::string &chronicle_name)
{
    if(!isConnected)
    {
        return -1; // Connection is not established
    }

    // Iterate through the vector to find the chronicle with the given name
    for(auto it = chronicles.begin(); it != chronicles.end(); ++it)
    {
        if((*it)->name == chronicle_name)
        {
            // Chronicle found, remove it from the vector
            chronicles.erase(it);
            return 0; // Success
        }
    }
    return -1; // Chronicle not found
}


std::pair <int, StoryHandle*>
ChronoEmulator::AcquireStory(const std::string &chronicle_name, const std::string &story_name
                             , const std::unordered_map <std::string, std::string> &attrs, int &flags)
{
    if(!isConnected)
    {
        return {-1, nullptr}; // Connection is not established
    }

    std::shared_ptr <Chronicle> foundChronicle = nullptr;

    // Find the chronicle by name
    for(auto &chronicle: chronicles)
    {
        if(chronicle->name == chronicle_name)
        {
            foundChronicle = chronicle;
            break; // Chronicle found
        }
    }

    if(!foundChronicle)
    {
        return {-1, nullptr}; // Chronicle does not exist
    }

    Chronicle &chronicle = *foundChronicle;

    bool exists = chronicle.storyExists(story_name);
    if(!exists)
    {
        chronicle.addStory(story_name);
    }
    auto story = chronicle.acquireStory(story_name);
    if(story == nullptr)
    {
        return {-1, story};
    }
    else
    {
        return {0, story};
    }
}


int ChronoEmulator::ReleaseStory(const std::string &chronicle_name, const std::string &story_name)
{
    if(!isConnected)
    {
        return -1; // Connection is not established
    }

    // Find the chronicle by name
    for(auto &chronicle: chronicles)
    {
        if(chronicle->name == chronicle_name)
        {
            bool exists = chronicle->storyExists(story_name);
            if(exists)
            {
                return chronicle->releaseStory(story_name);
            }
            else
            {
                return -1;
            }
        }
    }
    return -1; // Chronicle not found
}

int ChronoEmulator::DestroyStory(const std::string &chronicle_name, const std::string &story_name)
{
    if(!isConnected)
    {
        return -1; // Connection is not established
    }

    // Iterate through the chronicles to find the one with the given name
    for(auto &chronicle: chronicles)
    {
        if(chronicle->name == chronicle_name)
        {
            if(chronicle->storyExists(story_name))
            {
                return chronicle->deleteStory(story_name);
            }
            return 0;
        }
    }

    return -1; // Chronicle not found
}

}