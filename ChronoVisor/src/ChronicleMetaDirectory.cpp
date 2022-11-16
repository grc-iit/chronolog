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
    LOGD("%s constructor is called", typeid(*this).name());
//    chronicleMap_ = ChronoLog::Singleton<std::unordered_map<std::string, Chronicle *>>::GetInstance();
    chronicleMap_ = new std::unordered_map<std::string, Chronicle *>();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    LOGD("creating Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
//    extern std::shared_ptr<std::unordered_map<std::string, Chronicle *>> g_chronicleMap;
    auto res = chronicleMap_->emplace(std::to_string(cid), pChronicle);
    t2 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::nano> duration = (t2 - t1);
    LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
    return res.second;
}

bool ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                              const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Chronicle name=%s", name.c_str());
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
    LOGD("destroying Chronicle name=%s", name.c_str());
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
    LOGD("acquiring Chronicle name=%s", name.c_str());
    return CityHash64(name.c_str(), name.size());
}

bool ChronicleMetaDirectory::release_chronicle(uint64_t cid, int flags) {
    LOGD("releasing Chronicle cid=%lu", cid);
    return false;
}

bool ChronicleMetaDirectory::create_story(uint64_t cid, const std::string& name,
                                          const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle cid=%lu", name.c_str(), cid);
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    auto chronicleRecord = chronicleMap_->find(std::to_string(cid));
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        LOGD("Chronicle@%p", &(*pChronicle));
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
    LOGD("destroying Story name=%s in Chronicle cid=%lu", name.c_str(), cid);
    Chronicle *pChronicle = chronicleMap_->find(std::to_string(cid))->second;
    if (!pChronicle->removeStory(cid, name, flags)) {
        LOGE("Cannot find Story name=%s in Chronicle cid=%lu", name.c_str(), cid);
        return false;
    }
    return true;
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
    LOGD("acquiring Story name=%s in Chronicle cid=%lu", name.c_str(), cid);
    // add cid to name before hash to allow same story name across chronicles
    std::string story_name_for_hash = std::to_string(cid) + name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    LOGD("new Story sid=%lu", sid);
    return sid;
}

bool ChronicleMetaDirectory::release_story(uint64_t sid, int flags) {
    LOGD("releasing Story sid=%lu", sid);
    return false;
}

uint64_t ChronicleMetaDirectory::record_event(uint64_t sid, void *data) {
    LOGD("recording Event to Story sid=%lu", sid);
    return 0;
}

uint64_t ChronicleMetaDirectory::playback_event(uint64_t sid) {
    LOGD("playing back Event from Story sid=%lu", sid);
    return 0;
}

std::string ChronicleMetaDirectory::get_chronicle_attr(uint64_t &cid, const std::string& key) {
    LOGD("getting attributes key=%s from Chronicle cid=%lu", key.c_str(), cid);
    Chronicle *pChronicle = chronicleMap_->at(std::to_string(cid));
    return pChronicle->getPropertyList().at(key);
}

bool ChronicleMetaDirectory::edit_chronicle_attr(uint64_t &cid, const std::string& key, std::string value) {
    LOGD("editing attributes key=%s from Chronicle cid=%lu", key.c_str(), cid);
    Chronicle *pChronicle = chronicleMap_->at(std::to_string(cid));
    pChronicle->getPropertyList().insert_or_assign(key, value);
    return true;
}