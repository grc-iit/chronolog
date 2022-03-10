//
// Created by kfeng on 3/6/22.
//

#include <iostream>
#include <unordered_map>
#include <random>
#include <ChronicleMetaDirectory.h>
#include <TimeManager.h>
#include <city.h>

#define CHRONICLE_NAME_LEN 8
#define CHRONICLEMAP_SIZE 2
#define STORY_NAME_LEN 8
#define STORYMAP_SIZE 2
#define MAX_PRIORITY 5
#define EVENTMAP_SIZE 128
#define MAX_EVENT_SIZE 65536

std::random_device rd;
std::seed_seq ssq{rd()};
std::default_random_engine mt{rd()};
std::uniform_int_distribution<int> dist(0, INT32_MAX);

std::string gen_random(const int len) {
    static const char alphanum[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i) {
        tmp_s += alphanum[dist(mt) % (sizeof(alphanum) - 1)];
    }

    return tmp_s;
}

int main() {
    TimeManager *pTimeManager = new TimeManager();
    pTimeManager->setClocksourceType(ClocksourceType::C_STYLE);
    Clocksource *pClocksource = pTimeManager->clocksource_;
    std::string chronicle_names[CHRONICLEMAP_SIZE];
    std::string story_names[CHRONICLEMAP_SIZE * STORYMAP_SIZE];

    ChronicleMetaDirectory *pChronicleMetaDirectory = new ChronicleMetaDirectory();
    for (int i = 0; i < CHRONICLEMAP_SIZE; i++) {
        chronicle_names[i] = gen_random(CHRONICLE_NAME_LEN);
        std::string chronicle_name = chronicle_names[i];
        std::unordered_map<std::string, std::string> attrs;
        pChronicleMetaDirectory->create_chronicle(chronicle_name);
        uint64_t cid = pChronicleMetaDirectory->acquire_chronicle(chronicle_name, 0);

        for (int j = i * STORYMAP_SIZE; j < (i + 1) * STORYMAP_SIZE; j++) {
            story_names[j] = gen_random(STORY_NAME_LEN);
            std::string story_name = story_names[j];
            pChronicleMetaDirectory->create_story(cid, story_name);
            uint64_t sid = pChronicleMetaDirectory->acquire_story(cid, story_name, 0);
        }
    }

    std::unordered_map<std::string, Chronicle *> chronicleMap = pChronicleMetaDirectory->getChronicleMap();
    std::cout << "#entries in ChronicleMap: " << chronicleMap.size() << std::endl;
    for (auto const & chronicleRecord : chronicleMap) {
        std::unordered_map<std::string, Story *> storyMap = chronicleRecord.second->getStoryMap();
        for (auto const & storyRecord: storyMap) {
            std::cout << *storyRecord.second << std::endl;
        }
    }

    for (int i = 0; i < CHRONICLEMAP_SIZE; i++) {
        uint64_t cid = pChronicleMetaDirectory->acquire_chronicle(chronicle_names[i], 0);

        for (int j = i * STORYMAP_SIZE; j < (i + 1) * STORYMAP_SIZE; j++) {
            pChronicleMetaDirectory->destroy_story(cid, story_names[j], 0);
        }

        pChronicleMetaDirectory->destroy_chronicle(chronicle_names[i], 0);
    }
    delete pChronicleMetaDirectory;
    delete pTimeManager;

    return 0;
}