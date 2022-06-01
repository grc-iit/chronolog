//
// Created by kfeng on 3/9/22.
//

#include <ChronicleMetaDirectory.h>
#include <city.h>
#include <log.h>
#include <uuid/uuid.h>
#include <iostream>
#include <chrono>
#include <singleton.h>
#include <cassert>
#include <unistd.h>

ChronicleMetaDirectory::ChronicleMetaDirectory() {
    chronicleMap_ = ChronoLog::Singleton<std::unordered_map<std::string, Chronicle *>>::GetInstance();
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(std::to_string(cid), pChronicle);
    t2 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::nano> duration = (t2 - t1);
    LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
    return res.second;
}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                              const std::unordered_map<std::string, std::string>& attrs) {
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setProperty(attrs);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(std::to_string(cid), pChronicle);
    if (chronicleMap_->size() % 10 == 0)
        LOGD("10 chronicles have been created");
    return res.second;
}

bool ChronicleMetaDirectory::destroy_chronicle(const std::string& name, int flags) {
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(name.c_str(), name.size());
    auto chronicleRecord = chronicleMap_->find(std::to_string(cid));
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        delete pChronicle;
        auto nErased = chronicleMap_->erase(std::to_string(cid));
        if (chronicleMap_->size() % 10 == 0)
            LOGD("10 chronicles have been destroyed");
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
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

bool ChronicleMetaDirectory::create_story(uint64_t cid, const std::string& name,
                                          const std::unordered_map<std::string, std::string>& attrs) {
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    auto chronicleRecord = chronicleMap_->find(std::to_string(cid));
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        LOGD("Chronicle at address %lu", &(*pChronicle));
        bool res = pChronicle->addStory(cid, name, attrs);
        if (pChronicle->getStoryMapSize() % 10 == 0)
            LOGD("10 stories have been created");
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        return res;
    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
        return false;
    }
}

bool ChronicleMetaDirectory::destroy_story(uint64_t cid, const std::string& name, int flags) {
    Chronicle *pChronicle = chronicleMap_->find(std::to_string(cid))->second;
    if (!pChronicle->removeStory(cid, name, flags)) {
        LOGE("Cannot find Story name=%s in Chronicle cid=%lu", name.c_str(), cid);
        return false;
    }
}

std::vector<std::string> ChronicleMetaDirectory::get_story_list(uint64_t cid) {
    std::vector<std::string> storyList;
    Chronicle *pChronicle = chronicleMap_->find(std::to_string(cid))->second;
    for (auto const& story : pChronicle->getStoryMap()) {
        storyList.emplace_back(story.first);
    }
    return storyList;
}

uint64_t ChronicleMetaDirectory::acquire_story(uint64_t cid, const std::string& name, int flags) {
    // add cid to name before hash to allow same story name across chronicles
    std::string story_name_for_hash = std::to_string(cid) + name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
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

std::string ChronicleMetaDirectory::get_chronicle_attr(uint64_t &cid, const std::string& key) {
    Chronicle *pChronicle = chronicleMap_->at(std::to_string(cid));
    return pChronicle->getPropertyList().at(key);
}

bool ChronicleMetaDirectory::edit_chronicle_attr(uint64_t &cid, const std::string& key, std::string value) {
    Chronicle *pChronicle = chronicleMap_->at(std::to_string(cid));
    pChronicle->getPropertyList().insert_or_assign(key, value);
    return true;
}