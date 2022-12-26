//
// Created by kfeng on 3/9/22.
//

#include <ChronicleMetaDirectory.h>
#include <city.h>
#include <chrono>
#include <unistd.h>
#include <mutex>
#include <errcode.h>

ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
//    chronicleMap_ = ChronoLog::Singleton<std::unordered_map<std::string, Chronicle *>>::GetInstance();
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredChronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredStoryMap_ = new std::unordered_map<uint64_t, Story *>();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
    delete acquiredChronicleMap_;
    delete acquiredStoryMap_;
}

int ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    LOGD("creating Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(cid, pChronicle);
    t2 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::nano> duration = (t2 - t1);
    LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                             const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Chronicle name=%s", name.c_str());
    Chronicle *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setProperty(attrs);
    uint64_t cid = CityHash64(name.c_str(), name.size());
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(cid, pChronicle);
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::destroy_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("destroying Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(name.c_str(), name.size());
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        delete pChronicle;
        auto nErased = chronicleMap_->erase(cid);
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        if (nErased == 1) {
            return CL_SUCCESS;
        } else {
            return CL_ERR_UNKNOWN;
        }
    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
        return CL_ERR_NOT_EXIST;
    }
}

int ChronicleMetaDirectory::acquire_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("acquiring Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    Chronicle *pChronicle;
    try {
        pChronicle = chronicleMap_->at(cid);
    } catch (const std::out_of_range& e) {
        return CL_ERR_NOT_EXIST;
    }
    auto res = acquiredChronicleMap_->emplace(cid, pChronicle);
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::release_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("releasing Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    auto nErased = acquiredChronicleMap_->erase(cid);
    if (nErased == 1) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::create_story(std::string& chronicle_name,
                                         const std::string& story_name,
                                         const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        bool res = pChronicle->addStory(chronicle_name, story_name, attrs);
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        return res ? CL_SUCCESS : CL_ERR_UNKNOWN;
    } else {
        LOGE("Cannot find Chronicle name=%s", chronicle_name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

int ChronicleMetaDirectory::destroy_story(std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("destroying Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    Chronicle *pChronicle = chronicleMap_->find(cid)->second;
    if (!pChronicle->removeStory(chronicle_name, story_name, flags)) {
        LOGE("Cannot find Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
        return CL_ERR_UNKNOWN;
    }
    return CL_SUCCESS;
}

int ChronicleMetaDirectory::get_story_list(std::string& chronicle_name, std::vector<std::string>& story_name_list) {
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    Chronicle *pChronicle = chronicleMap_->find(cid)->second;
    for (auto const& story : pChronicle->getStoryMap()) {
        //TODO: need SID to name mapping
        story_name_list.emplace_back(std::to_string(story.first));
    }
    return CL_SUCCESS;
}

int ChronicleMetaDirectory::acquire_story(const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("acquiring Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    // add cid to name before hash to allow same story name across chronicles
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    Chronicle *pChronicle;
    try {
        pChronicle = chronicleMap_->at(cid);
    } catch (const std::out_of_range& e) {
        return CL_ERR_NOT_EXIST;
    }
    Story *pStory;
    try {
        pStory = pChronicle->getStoryMap().at(sid);
    } catch (const std::out_of_range& e) {
        return CL_ERR_NOT_EXIST;
    }
    auto res = acquiredStoryMap_->emplace(sid, pStory);
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::release_story(const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("releasing Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    auto nErased = acquiredStoryMap_->erase(sid);
    if (nErased == 1) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_NOT_EXIST;
    }
}

uint64_t ChronicleMetaDirectory::record_event(uint64_t sid, void *data) {
    LOGD("recording Event to Story sid=%lu", sid);
    return 0;
}

uint64_t ChronicleMetaDirectory::playback_event(uint64_t sid) {
    LOGD("playing back Event from Story sid=%lu", sid);
    return 0;
}

int ChronicleMetaDirectory::get_chronicle_attr(std::string& name, const std::string& key, std::string& value) {
    LOGD("getting attributes key=%s from Chronicle name=%s", key.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    Chronicle *pChronicle;
    try {
        pChronicle = chronicleMap_->at(cid);
    } catch (const std::out_of_range& e) {
        return CL_ERR_NOT_EXIST;
    }
    if (pChronicle) {
        value = pChronicle->getPropertyList().at(key);
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ChronicleMetaDirectory::edit_chronicle_attr(std::string& name,
                                                const std::string& key,
                                                const std::string& value) {
    LOGD("editing attribute key=%s, value=%s from Chronicle name=%s", key.c_str(), value.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    Chronicle *pChronicle;
    try {
        pChronicle = chronicleMap_->at(cid);
    } catch (const std::out_of_range& e) {
        return CL_ERR_NOT_EXIST;
    }
    auto res = pChronicle->getPropertyList().insert_or_assign(key, value);
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}