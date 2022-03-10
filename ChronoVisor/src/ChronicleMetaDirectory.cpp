//
// Created by kfeng on 3/9/22.
//

#include <ChronicleMetaDirectory.h>
#include <city.h>
#include <log.h>

ChronicleMetaDirectory::ChronicleMetaDirectory() {

}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    chronicleMap_.emplace(std::to_string(cid), pChronicle);
    return true;
}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                              const std::unordered_map<std::string, std::string>& attrs) {
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setProperty(attrs);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    chronicleMap_.emplace(std::to_string(cid), pChronicle);
    return true;
}

bool ChronicleMetaDirectory::destroy_chronicle(const std::string& name, int flags) {
    uint64_t cid = CityHash64(name.c_str(), name.size());
    auto chronicleRecord = chronicleMap_.find(std::to_string(cid));
    if (chronicleRecord != chronicleMap_.end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        delete pChronicle;
        auto nErased = chronicleMap_.erase(name);
        return (nErased == 1);
    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
        return false;
    }
}

uint64_t ChronicleMetaDirectory::acquire_chronicle(const std::string& name, int flags) {
    return CityHash64(name.c_str(), name.size());
}

bool ChronicleMetaDirectory::release_chronicle(uint64_t cid, int flags) {
    return false;
}

bool ChronicleMetaDirectory::create_story(uint64_t cid, const std::string& name) {
    Story *pStory = new Story();
    pStory->setName(name);
    uint64_t sid = CityHash64(name.c_str(), name.size());
    pStory->setSid(sid);
    pStory->setCid(cid);
    auto chronicleRecord = chronicleMap_.find(std::to_string(cid));
    if (chronicleRecord != chronicleMap_.end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        pChronicle->getStoryMap().emplace(std::to_string(sid), pStory);
        return true;
    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
        return false;
    }
}

bool ChronicleMetaDirectory::destroy_story(uint64_t cid, const std::string& name, int flags) {
    uint64_t sid = CityHash64(name.c_str(), name.size());
    Chronicle *pChronicle = chronicleMap_.find(std::to_string(cid))->second;
    std::unordered_map<std::string, Story *> storyMap = pChronicle->getStoryMap();
    auto storyRecord = storyMap.find(std::to_string(sid));
    if (storyRecord != storyMap.end()) {
        Story *pStory = storyRecord->second;
        delete pStory;
        auto nErased = storyMap.erase(std::to_string(sid));
        return (nErased == 1);
    } else {
        LOGE("Cannot find Story sid=%lu", sid);
        return false;
    }
}

std::vector<std::string> ChronicleMetaDirectory::get_story_list(uint64_t cid) {
    std::vector<std::string> storyList;
    Chronicle *pChronicle = chronicleMap_.find(std::to_string(cid))->second;
    for (auto const& story : pChronicle->getStoryMap()) {
        storyList.push_back(story.first);
    }
    return storyList;
}

uint64_t ChronicleMetaDirectory::acquire_story(uint64_t cid, const std::string& name, int flags) {
    return CityHash64(name.c_str(), name.size());
}

bool ChronicleMetaDirectory::release_story(uint64_t sid, int flags) {
    return false;
}

uint64_t ChronicleMetaDirectory::record_event(uint64_t sid, void *data) {
    return 0;
}

uint64_t ChronicleMetaDirectory::playback_event(uint64_t sid) {
    return 0;
}

std::string ChronicleMetaDirectory::get_chronicle_attr(uint64_t cid, const std::string& key) {
    Chronicle *pChronicle = chronicleMap_.at(std::to_string(cid));
    return pChronicle->getPropertyList().at(key);
}

bool ChronicleMetaDirectory::edit_chronicle_attr(uint64_t cid, const std::string& key, std::string value) {
    Chronicle *pChronicle = chronicleMap_.at(std::to_string(cid));
    pChronicle->getPropertyList().insert_or_assign(key, value);
    return true;
}