//
// Created by kfeng on 3/6/22.
//

#include <iostream>
#include <unordered_map>
#include <random>
#include <ChronicleMetaDirectory.h>
#include "TimeManager.h"
#include "city.h"
#include <common.h>

#define CHRONICLE_NAME_LEN 8
#define CHRONICLEMAP_SIZE 2
#define STORY_NAME_LEN 8
#define STORYMAP_SIZE 2
#define MAX_PRIORITY 5
#define EVENTMAP_SIZE 128
#define MAX_EVENT_SIZE 65536

int main()
{
    TimeManager*pTimeManager = new TimeManager();
    pTimeManager->setClocksourceType(ClocksourceType::C_STYLE);
    Clocksource*pClocksource = pTimeManager->clocksource_;
    std::string chronicle_names[CHRONICLEMAP_SIZE];
    std::string story_names[CHRONICLEMAP_SIZE * STORYMAP_SIZE];

    ChronicleMetaDirectory*pChronicleMetaDirectory = new ChronicleMetaDirectory();
    for(int i = 0; i < CHRONICLEMAP_SIZE; i++)
    {
        chronicle_names[i] = gen_random(CHRONICLE_NAME_LEN);
        std::string chronicle_name = chronicle_names[i];
        std::unordered_map <std::string, std::string> attrs;
        pChronicleMetaDirectory->create_chronicle(chronicle_name);
        uint64_t cid = pChronicleMetaDirectory->acquire_chronicle(chronicle_name, 0);

        for(int j = i * STORYMAP_SIZE; j < (i + 1) * STORYMAP_SIZE; j++)
        {
            story_names[j] = gen_random(STORY_NAME_LEN);
            std::string story_name = story_names[j];
            std::unordered_map <std::string, std::string> story_attrs;
            pChronicleMetaDirectory->create_story(chronicle_name, story_name, story_attrs);
            uint64_t sid = pChronicleMetaDirectory->acquire_story(chronicle_name, story_name, 0);
        }
    }

    auto chronicleMap = pChronicleMetaDirectory->getChronicleMap();
    LOGD("Number of entries in ChronicleMap: {}", chronicleMap->size());
    for(auto const &chronicleRecord: *chronicleMap)
    {
        std::unordered_map <uint64_t, Story*> storyMap = chronicleRecord.second->getStoryMap();
        for(auto const &storyRecord: storyMap)
        {
            std::stringstream ss;
            ss << *storyRecord.second;
            LOGD(" Story record in seconds: {}", ss.str());
        }
    }

    for(int i = 0; i < CHRONICLEMAP_SIZE; i++)
    {
        uint64_t cid = pChronicleMetaDirectory->acquire_chronicle(chronicle_names[i], 0);

        for(int j = i * STORYMAP_SIZE; j < (i + 1) * STORYMAP_SIZE; j++)
        {
            pChronicleMetaDirectory->destroy_story(chronicle_names[i], story_names[j], 0);
        }

        pChronicleMetaDirectory->destroy_chronicle(chronicle_names[i], 0);
    }
    delete pChronicleMetaDirectory;
    delete pTimeManager;

    return 0;
}