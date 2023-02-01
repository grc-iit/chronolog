//
// Created by kfeng on 3/9/22.
//

#include <ChronicleMetaDirectory.h>
#include "city.h"
#include <chrono>
#include <unistd.h>
#include <mutex>
#include <typedefs.h>
#include <iostream>

extern std::mutex g_chronicleMetaDirectoryMutex_;       /* protects global ChronicleMap */
extern std::mutex g_acquiredChronicleMapMutex_;         /* protects global AcquiredChronicleMap */
extern std::mutex g_acquiredStoryMapMutex_;             /* protects global AcquiredStoryMap */

ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredChronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredStoryMap_ = new std::unordered_map<uint64_t, Story *>();
    chronicleName2IdMap_ = new std::unordered_map<std::string, uint64_t>();
    chronicleId2NameMap_ = new std::unordered_map<uint64_t, std::string>();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
    delete acquiredChronicleMap_;
    delete acquiredStoryMap_;
    delete chronicleName2IdMap_;
    delete chronicleId2NameMap_;
}

/**
 * Create a Chronicle
 * @param name: name of the Chronicle
 * @return CL_SUCCESS if succeed to create the Chronicle
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    std::unordered_map<std::string, std::string> attrs;
    return create_chronicle(name, attrs);
}

/**
 * Create a Chronicle
 * @param name: name of the Chronicle
 * @param attrs: attributes associated with the Chronicle
 * @return CL_SUCCESS if succeed to create the Chronicle
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                             const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* Check if Chronicle already exists, fail if true */
    uint64_t cid;
    auto name2IdRecord = chronicleName2IdMap_->find(name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        return CL_ERR_CHRONICLE_EXISTS;
    } else {
        cid = CityHash64(name.c_str(), name.length());
    }
    auto *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(cid, pChronicle);
    chronicleName2IdMap_->insert_or_assign(name, cid);
    chronicleId2NameMap_->insert_or_assign(cid, name);
    t2 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::nano> duration = (t2 - t1);
    LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
    if (res.second) {
        return CL_SUCCESS;
    } else {
        return CL_ERR_UNKNOWN;
    }
}

/**
 * Destroy a Chronicle
 * No need to check its Stories. Users are required to release all Stories before releasing a Chronicle
 * @param name: name of the Chronicle
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Chronicle
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_ACQUIRED if the Chronicle is acquired by others and cannot be destroyed
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::destroy_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("destroying Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        uint64_t cid = name2IdRecord->second;
        /* Check if Chronicle is acquired, fail if true */
        std::lock_guard<std::mutex> acquiredChronicleMapLock(g_acquiredChronicleMapMutex_);
        if (acquiredStoryMap_->find(cid) != acquiredStoryMap_->end())
            return CL_ERR_ACQUIRED;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle->getAcquisitionCount() != 0) {
            return CL_ERR_UNKNOWN;
        }
        delete pChronicle;
        auto nErased = chronicleMap_->erase(cid);
        chronicleName2IdMap_->erase(name);
        chronicleId2NameMap_->erase(cid);
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        if (nErased == 1) {
            return CL_SUCCESS;
        } else {
            return CL_ERR_UNKNOWN;
        }
    } else {
        LOGE("Cannot find Chronicle name=%s", name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Acquire a Chronicle
 * @param name: name of the Chronicle
 * @param flags: flags
 * @return CL_SUCCESS if succeed to acquire the Chronicle
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::acquire_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("acquiring Chronicle name=%s", name.c_str());
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        uint64_t cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        pChronicle->incrementAcquisitionCount();
        std::lock_guard<std::mutex> lock(g_acquiredChronicleMapMutex_);
        /* Emplace only when Chronicle is not already acquired */
        if (acquiredStoryMap_->find(cid) == acquiredStoryMap_->end()) {
            auto res = acquiredChronicleMap_->emplace(cid, pChronicle);
            if (res.second) {
                return CL_SUCCESS;
            } else {
                return CL_ERR_UNKNOWN;
            }
        } else {
            return CL_SUCCESS;
        }
    } else {
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Release a Chronicle
 * @param name: name of the Chronicle
 * @param flags: flags
 * @return CL_SUCCESS if succeed to release the Chronicle
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::release_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("releasing Chronicle name=%s", name.c_str());
    Chronicle *pChronicle;
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    {
        /* First check if Chronicle exists, fail if false */
        auto name2IdRecord = chronicleName2IdMap_->find(name);
        if (name2IdRecord != chronicleName2IdMap_->end()) {
            cid = name2IdRecord->second;
            pChronicle = chronicleMap_->find(cid)->second;
            /* Decrement AcquisitionCount */
            pChronicle->decrementAcquisitionCount();
        } else {
            return CL_ERR_NOT_EXIST;
        }
    }
    /* Remove it from AcquiredChronicleMap when it's not acquired by anyone */
    if (pChronicle->getAcquisitionCount() == 0) {
        std::lock_guard<std::mutex> lock(g_acquiredChronicleMapMutex_);
        auto nErased = acquiredChronicleMap_->erase(cid);
        if (nErased == 1) {
            return CL_SUCCESS;
        } else {
            return CL_ERR_UNKNOWN;
        }
    } else {
        return CL_SUCCESS;
    }
}

/**
 * Create a Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param attrs: attributes associated with the Story
 * @return CL_SUCCESS if succeed to create the Story
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_STORY_EXISTS if a Story with the same name already exists
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_story(std::string& chronicle_name,
                                         const std::string& story_name,
                                         const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid;
    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        /* Ask Chronicle to create the Story */
        CL_Status res = pChronicle->addStory(chronicle_name, cid, story_name, attrs);
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        /* Forward its return value */
        return res;
    } else {
        LOGE("Cannot find Chronicle name=%s", chronicle_name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Destroy a Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Story
 *         CL_ERR_ACQUIRED if the Story is acquired by others and cannot be destroyed
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::destroy_story(std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("destroying Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) return CL_ERR_NOT_EXIST;
        /* Then check if Story is acquired, fail if true */
        std::lock_guard<std::mutex> acquiredStoryMapLock(g_acquiredStoryMapMutex_);
        if (acquiredStoryMap_->find(sid) != acquiredStoryMap_->end())
            return CL_ERR_ACQUIRED;
        /* Ask the Chronicle to destroy the Story */
        CL_Status res = pChronicle->removeStory(chronicle_name, story_name, flags);
        if (res != CL_SUCCESS) {
            LOGE("Fail to remove Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
        }
        return res;
    } else {
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Acquire a Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Story
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::acquire_story(const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("acquiring Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) return CL_ERR_NOT_EXIST;
        Story *pStory = pChronicle->getStoryMap().find(sid)->second;
        /* Increment AcquisitionCount */
        pStory->incrementAcquisitionCount();
        std::lock_guard<std::mutex> acquiredStoryMapLock(g_acquiredStoryMapMutex_);
        /* Emplace only when the Story is not already acquired */
        if (acquiredStoryMap_->find(sid) == acquiredStoryMap_->end()) {
            auto res = acquiredStoryMap_->emplace(sid, pStory);
            if (res.second) {
                return CL_SUCCESS;
            } else {
                return CL_ERR_UNKNOWN;
            }
        } else {
            return CL_SUCCESS;
        }
    } else {
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Release a Story
 * @param cchronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Story
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::release_story(const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("releasing Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) return CL_ERR_NOT_EXIST;
        Story *pStory = pChronicle->getStoryMap().find(sid)->second;
        pStory->decrementAcquisitionCount();
        std::lock_guard<std::mutex> AcquiredStoryMapLock(g_acquiredStoryMapMutex_);
        /* Remove Story from AcquiredStoryMap when its AcquisitionCount reduces to zero */
        if (pStory->getAcquisitionCount() == 0) {
            auto nErased = acquiredStoryMap_->erase(sid);
            if (nErased == 1) {
                return CL_SUCCESS;
            } else {
                return CL_ERR_UNKNOWN;
            }
        } else {
            return CL_SUCCESS;
        }
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
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle) {
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) {
                value = propertyRecord->second;
                return CL_SUCCESS;
            } else
                return CL_ERR_NOT_EXIST;
        } else
            return CL_ERR_UNKNOWN;
    } else
        return CL_ERR_NOT_EXIST;
}

int ChronicleMetaDirectory::edit_chronicle_attr(std::string& name,
                                                const std::string& key,
                                                const std::string& value) {
    LOGD("editing attribute key=%s, value=%s from Chronicle name=%s", key.c_str(), value.c_str(), name.c_str());
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle) {
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) {
                auto res = pChronicle->getPropertyList().insert_or_assign(key, value);
                if (res.second)
                    return CL_SUCCESS;
                else
                    return CL_ERR_UNKNOWN;
            } else
                return CL_ERR_NOT_EXIST;
        } else
            return CL_ERR_UNKNOWN;
    } else
        return CL_ERR_NOT_EXIST;
}

/**
 * Show all the Chronicles names
 * @param client_id: Client ID
 * @return a vector of the names of all Chronicles
 */
std::vector<std::string> ChronicleMetaDirectory::show_chronicles(std::string &client_id) {
    LOGD("showing all Chronicles client %s", client_id.c_str());
    std::vector<std::string> chronicle_names;
    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    for (auto &[key, value] : *chronicleMap_) {
        chronicle_names.emplace_back(chronicleId2NameMap_->find(key)->second);
    }
    return chronicle_names;
}

/**
 * Show all the Stories names in a Chronicle
 * @param client_id: Client ID
 * @param chronicle_name: name of the Chronicle
 * @return a vector of the names of given Chronicle
 *         empty vector if Chronicle does not exist
 */
std::vector<std::string>
ChronicleMetaDirectory::show_stories(std::string &client_id, const std::string &chronicle_name) {
    LOGD("showing all Stories in Chronicle name=%s for client %s", chronicle_name.c_str(), client_id.c_str());
    std::vector<std::string> story_names;
    uint64_t cid;
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
    if (name2IdRecord != chronicleName2IdMap_->end()) {
        cid = name2IdRecord->second;
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        for (auto &[key, value] : pChronicle->getStoryMap()) {
            story_names.emplace_back(pChronicle->getId2NameMap()->find(key)->second);
        }
    }
    return story_names;
}
