#ifndef CHRONOKVS_MOCK_CHRONOLOG_H
#define CHRONOKVS_MOCK_CHRONOLOG_H

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <memory>
#include "../src/story.h"
#include "../src/chronicle.h"

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

    /*
    int GetChronicleAttr(const std::string &chronicle_name, const std::string &key, std::string &value);

    int EditChronicleAttr(const std::string &chronicle_name, const std::string &key, const std::string &value);

    std::vector <std::string> &ShowChronicles(std::vector <std::string> &output);

    std::vector <std::string> &ShowStories(const std::string &chronicle_name, std::vector <std::string> &output);
    */
};

#endif //CHRONOKVS_MOCK_CHRONOLOG_H
