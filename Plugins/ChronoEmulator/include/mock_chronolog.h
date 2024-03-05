#ifndef CHRONOKVS_MOCK_CHRONOLOG_H
#define CHRONOKVS_MOCK_CHRONOLOG_H

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <unordered_map>
#include "../src/story.h"
#include "../src/chronicle.h"

class Story {
private:
    StoryHandle* storyHandle;

public:
    uint64_t record(const std::string &event){
        return storyHandle->record(event);
    }

    std::vector <std::string> replay(uint64_t timestamp){
        return storyHandle->replay(timestamp);
    }
};

class MockChronolog: public std::enable_shared_from_this <MockChronolog>
{
private:
    std::vector <std::shared_ptr <Chronicle>> chronicles;
    bool isConnected = false;

public:
    int Connect();

    int Disconnect();

    int CreateChronicle(const std::string &chronicle_name, const std::unordered_map <std::string, std::string> &attrs
                        , int &flags);

    int DestroyChronicle(const std::string &chronicle_name);

    std::pair <int, StoryHandle*> AcquireStory(const std::string &chronicle_name, const std::string &story_name
                                               , const std::unordered_map <std::string, std::string> &attrs
                                               , int &flags);

    int ReleaseStory(const std::string &chronicle_name, const std::string &story_name);

    int DestroyStory(const std::string &chronicle_name, const std::string &story_name);
};

#endif //CHRONOKVS_MOCK_CHRONOLOG_H
