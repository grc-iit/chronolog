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
#include <mutex>

ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
//    chronicleMap_ = ChronoLog::Singleton<std::unordered_map<std::string, Chronicle *>>::GetInstance();
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
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
    extern std::mutex g_chronicleMetaDirectoryMutex_;
//    extern std::shared_ptr<std::unordered_map<std::string, Chronicle *>> g_chronicleMap;
    g_chronicleMetaDirectoryMutex_.lock();
    auto res = chronicleMap_->emplace(cid, pChronicle);
    g_chronicleMetaDirectoryMutex_.unlock();
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
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    auto res = chronicleMap_->emplace(cid, pChronicle);
    g_chronicleMetaDirectoryMutex_.unlock();
    if (chronicleMap_->size() % 10 == 0)
        LOGD("10 chronicles have been created");
    return res.second;
}

bool ChronicleMetaDirectory::destroy_chronicle(const std::string& name, int flags) {
    LOGD("destroying Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(name.c_str(), name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        delete pChronicle;
        auto nErased = chronicleMap_->erase(cid);
        if (chronicleMap_->size() % 10 == 0)
            LOGD("10 chronicles have been destroyed");
        g_chronicleMetaDirectoryMutex_.unlock();
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        return (nErased == 1);
    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
        g_chronicleMetaDirectoryMutex_.unlock();
        return false;
    }
}

uint64_t ChronicleMetaDirectory::acquire_chronicle(const std::string& name, int flags) {
    LOGD("acquiring Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Chronicle *c = chronicleMap_->at(cid);
    g_chronicleMetaDirectoryMutex_.unlock();
    extern std::mutex g_acquiredChronicleMapMutex_;
    g_acquiredChronicleMapMutex_.lock();
    acquiredChronicleMap_->emplace(cid, c);
    g_acquiredChronicleMapMutex_.unlock();
    return cid;
}

bool ChronicleMetaDirectory::release_chronicle(const std::string& name, int flags) {
    LOGD("releasing Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    extern std::mutex g_acquiredChronicleMapMutex_;
    g_acquiredChronicleMapMutex_.lock();
    return (acquiredChronicleMap_->erase(cid) == 1);
    g_acquiredChronicleMapMutex_.unlock();
}

bool ChronicleMetaDirectory::create_story(std::string &chronicle_name, const std::string& story_name,
                                          const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        bool res = pChronicle->addStory(chronicle_name, story_name, attrs);
        if (pChronicle->getStoryMapSize() % 10 == 0)
            LOGD("10 stories have been created");
        g_chronicleMetaDirectoryMutex_.unlock();
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        return res;
    } else {
        LOGE("Cannot find Chronicle name=%s", chronicle_name.c_str());
        g_chronicleMetaDirectoryMutex_.unlock();
        return false;
    }
}

bool ChronicleMetaDirectory::destroy_story(std::string &chronicle_name, const std::string& story_name, int flags) {
    LOGD("destroying Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Chronicle *pChronicle = chronicleMap_->find(cid)->second;
    g_chronicleMetaDirectoryMutex_.unlock();
    if (!pChronicle->removeStory(chronicle_name, story_name, flags)) {
        LOGE("Cannot find Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
        return false;
    }
    return true;
}

std::vector<uint64_t> ChronicleMetaDirectory::get_story_list(std::string &chronicle_name) {
    std::vector<uint64_t> storyList;
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Chronicle *pChronicle = chronicleMap_->find(cid)->second;
    g_chronicleMetaDirectoryMutex_.unlock();
    for (auto const& story : pChronicle->getStoryMap()) {
        storyList.emplace_back(story.first);
    }
    return storyList;
}

uint64_t ChronicleMetaDirectory::acquire_story(const std::string &chronicle_name, const std::string& story_name, int flags) {
    LOGD("acquiring Story name=%s in Chronicle cid=%lu", story_name.c_str(), chronicle_name.c_str());
    // add cid to name before hash to allow same story name across chronicles
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Story *s = chronicleMap_->at(cid)->getStoryMap().at(sid);
    g_chronicleMetaDirectoryMutex_.unlock();
    extern std::mutex g_acquiredStoryMapMutex_;
    g_acquiredStoryMapMutex_.lock();
    acquiredStoryMap_->emplace(sid, s);
    g_acquiredStoryMapMutex_.unlock();
    return sid;
}

bool
ChronicleMetaDirectory::release_story(const std::string &chronicle_name, const std::string &story_name, int flags) {
    LOGD("releasing Story name=%s in Chronicle cid=%lu", story_name.c_str(), chronicle_name.c_str());
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    extern std::mutex g_acquiredStoryMapMutex_;
    g_acquiredStoryMapMutex_.lock();
    acquiredStoryMap_->erase(sid);
    g_acquiredStoryMapMutex_.unlock();
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

std::string ChronicleMetaDirectory::get_chronicle_attr(std::string &name, const std::string& key) {
    LOGD("getting attributes key=%s from Chronicle name=%s", key.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Chronicle *pChronicle = chronicleMap_->at(cid);
    g_chronicleMetaDirectoryMutex_.unlock();
    return pChronicle->getPropertyList().at(key);
}

bool ChronicleMetaDirectory::edit_chronicle_attr(std::string &name, const std::string& key, std::string value) {
    LOGD("editing attribute key=%s, value=%s from Chronicle name=%s", key.c_str(), value.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    extern std::mutex g_chronicleMetaDirectoryMutex_;
    g_chronicleMetaDirectoryMutex_.lock();
    Chronicle *pChronicle = chronicleMap_->at(cid);
    g_chronicleMetaDirectoryMutex_.unlock();
    pChronicle->getPropertyList().insert_or_assign(key, value);
    return true;
}