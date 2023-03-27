//
// Created by kfeng on 3/9/22.
//

#include <ChronicleMetaDirectory.h>
#include "city.h"
#include <chrono>
#include <unistd.h>
#include <mutex>
#include <typedefs.h>


ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredStoryMap_ = new std::unordered_map<uint64_t, Story *>();
    acquiredChronicleClientMap_ = new std::unordered_multimap<uint64_t,std::string> ();
    acquiredStoryClientMap_ = new std::unordered_multimap<uint64_t,std::string> ();
//    chronicleName2IdMap_ = new std::unordered_map<std::string, uint64_t>();
//    chronicleId2NameMap_ = new std::unordered_map<uint64_t, std::string>();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
    delete acquiredStoryMap_;
    delete acquiredChronicleClientMap_;
    delete acquiredStoryClientMap_;
//    delete chronicleName2IdMap_;
//    delete chronicleId2NameMap_;
}

/**
 * Create a Chronicle
 * @param name: name of the Chronicle
 * @return CL_SUCCESS if succeed to create the Chronicle \n
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists \n
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
 * @return CL_SUCCESS if succeed to create the Chronicle \n
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_chronicle(const std::string& name,
                                             const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Chronicle name=%s", name.c_str());
    for (auto iter = attrs.begin(); iter != attrs.end(); ++iter) {
        LOGD("%s=%s", iter->first.c_str(), iter->second.c_str());
    }
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* Check if Chronicle already exists, fail if true */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        return CL_ERR_CHRONICLE_EXISTS;
//    } else {
//        cid = CityHash64(name.c_str(), name.length());
//    }
    cid = CityHash64(name.c_str(), name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        return CL_ERR_CHRONICLE_EXISTS;
    }
    auto *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setCid(cid);
    auto res = chronicleMap_->emplace(cid, pChronicle);
//    chronicleName2IdMap_->insert_or_assign(name, cid);
//    chronicleId2NameMap_->insert_or_assign(cid, name);
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
 * Destroy a Chronicle \n
 * No need to check its Stories. Users are required to release all Stories before releasing a Chronicle
 * @param name: name of the Chronicle
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Chronicle \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_ACQUIRED if the Chronicle is acquired by others and cannot be destroyed \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::destroy_chronicle(const std::string& name,
                                              int& flags) {
    LOGD("destroying Chronicle name=%s, flags=%d", name.c_str(), flags);
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(name.c_str(), name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        /* Check if Chronicle is acquired, fail if true */
        std::lock_guard<std::mutex> acquiredChronicleMapLock(g_acquiredChronicleMapMutex_);
        if (acquiredStoryMap_->find(cid) != acquiredStoryMap_->end()) {
            return CL_ERR_ACQUIRED;
        }
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle->getAcquisitionCount() != 0) {
            return CL_ERR_UNKNOWN;
        }
        delete pChronicle;
        auto nErased = chronicleMap_->erase(cid);
//        chronicleName2IdMap_->erase(name);
//        chronicleId2NameMap_->erase(cid);
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
 * Create a Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param attrs: attributes associated with the Story
 * @return CL_SUCCESS if succeed to create the Story \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_STORY_EXISTS if a Story with the same name already exists \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_story(std::string& chronicle_name,
                                         const std::string& story_name,
                                         const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
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
 * @return CL_SUCCESS if succeed to destroy the Story \n
 *         CL_ERR_ACQUIRED if the Story is acquired by others and cannot be destroyed \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::destroy_story(std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("destroying Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) {
            return CL_ERR_NOT_EXIST;
        }
        /* Then check if Story is acquired, fail if true */
        std::lock_guard<std::mutex> acquiredStoryMapLock(g_acquiredStoryMapMutex_);
        if (acquiredStoryMap_->find(sid) != acquiredStoryMap_->end()) {
            return CL_ERR_ACQUIRED;
        }
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
 * @return CL_SUCCESS if succeed to destroy the Story \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::acquire_story(const std::string &client_id,
                                          const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("client_id=%s acquiring Story name=%s in Chronicle name=%s, flags=%d",
         client_id.c_str(), story_name.c_str(), chronicle_name.c_str(), flags);
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    int ret = CL_ERR_NOT_EXIST;
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleMapRecord->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) {
            return CL_ERR_NOT_EXIST;
        }
        /* Last check if this client has acquired this Story already, return success if true */
        auto range = acquiredStoryClientMap_->equal_range(sid);
        bool exists = false;
        if (range.first != range.second) {
            for (auto t = range.first; t != range.second; ++t) {
                if (t->second.compare(client_id) == 0) {
                    exists = true;
                    break;
                }
            }
        }
        if (!exists) {
            /* All checks passed, increment AcquisitionCount */
            Story *pStory = pChronicle->getStoryMap().find(sid)->second;
            pStory->incrementAcquisitionCount();
            std::lock_guard<std::mutex> acquiredStoryMapLock(g_acquiredStoryMapMutex_);
            std::pair<uint64_t, std::string> p(sid, client_id);
            acquiredStoryClientMap_->insert(p);
            ret = CL_SUCCESS;
            if (acquiredStoryMap_->find(sid) == acquiredStoryMap_->end()) {
                auto res = acquiredStoryMap_->emplace(sid, pStory);
                if (res.second) {
                    ret = CL_SUCCESS;
                } else {
                    ret = CL_ERR_UNKNOWN;
                }
            }
        } else {
            ret = CL_ERR_UNKNOWN;
        }
        ret = CL_SUCCESS;
    } else {
        return CL_ERR_NOT_EXIST;
    }
    return ret;
}

/**
 * Release a Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @return CL_SUCCESS if succeed to destroy the Story \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::release_story(const std::string &client_id,
                                          const std::string& chronicle_name,
                                          const std::string& story_name,
                                          int& flags) {
    LOGD("client_id=%s releasing Story name=%s in Chronicle name=%s, flags=%d",
         client_id.c_str(), story_name.c_str(), chronicle_name.c_str(), flags);
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    int ret = CL_ERR_NOT_EXIST;
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        /* Then check if Story exists, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) {
            return CL_ERR_NOT_EXIST;
        }
        Story *pStory = pChronicle->getStoryMap().find(sid)->second;
        /* Decrement AcquisitionCount */
        auto range = acquiredStoryClientMap_->equal_range(sid);
        if (range.first != range.second) {
            for (auto t = range.first; t != range.second;) {
                if (t->second.compare(client_id) == 0) {
                    t = acquiredStoryClientMap_->erase(t);
                    pStory->decrementAcquisitionCount();
                    ret = CL_SUCCESS;
                    if (pStory->getAcquisitionCount() == 0) {
                        auto nErased = acquiredStoryMap_->erase(sid);
                        if (nErased == 1) ret = CL_SUCCESS;
                        else ret = CL_ERR_UNKNOWN;
                    }
                    break;
                } else ++t;
            }
        } else {
            ret = CL_ERR_UNKNOWN;
        }
    }
    return ret;
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
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(name.c_str(), name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle) {
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) {
                value = propertyRecord->second;
                return CL_SUCCESS;
            } else {
                return CL_ERR_NOT_EXIST;
            }
        } else {
            return CL_ERR_UNKNOWN;
        }
    } else {
        return CL_ERR_NOT_EXIST;
    }
}

int ChronicleMetaDirectory::edit_chronicle_attr(std::string& name,
                                                const std::string& key,
                                                const std::string& value) {
    LOGD("editing attribute key=%s, value=%s from Chronicle name=%s", key.c_str(), value.c_str(), name.c_str());
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(name.c_str(), name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        if (pChronicle) {
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) {
                auto res = pChronicle->getPropertyList().insert_or_assign(key, value);
                if (res.second) {
                    return CL_SUCCESS;
                } else {
                    return CL_ERR_UNKNOWN;
                }
            } else {
                return CL_ERR_NOT_EXIST;
            }
        } else {
            return CL_ERR_UNKNOWN;
        }
    } else {
        return CL_ERR_NOT_EXIST;
    }
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
        chronicle_names.emplace_back(value->getName());
    }
    return chronicle_names;
}

/**
 * Show all the Stories names in a Chronicle
 * @param client_id: Client ID
 * @param chronicle_name: name of the Chronicle
 * @return a vector of the names of given Chronicle \n
 *         empty vector if Chronicle does not exist
 */
std::vector<std::string>
ChronicleMetaDirectory::show_stories(std::string &client_id, const std::string &chronicle_name) {
    LOGD("showing all Stories in Chronicle name=%s for client %s", chronicle_name.c_str(), client_id.c_str());
    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    std::vector<std::string> story_names;
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        for (auto &[key, value] : pChronicle->getStoryMap()) {
            std::string story_name = value->getName();
//            story_names.emplace_back(story_name.erase(story_name.find(chronicle_name), chronicle_name.length()));
            story_names.emplace_back(story_name);
        }
    }
    return story_names;
}
