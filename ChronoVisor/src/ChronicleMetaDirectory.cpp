#include <ChronicleMetaDirectory.h>
#include "city.h"
#include <chrono>
#include <unistd.h>
#include <mutex>
#include <typedefs.h>
#include <ClientRegistryManager.h>

ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
//    chronicleName2IdMap_ = new std::unordered_map<std::string, uint64_t>();
//    chronicleId2NameMap_ = new std::unordered_map<uint64_t, std::string>();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
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
int ChronicleMetaDirectory::create_chronicle(const std::string &name) {
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
int ChronicleMetaDirectory::create_chronicle(const std::string &name,
                                             const std::unordered_map<std::string, std::string> &attrs) {
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
        LOGD("A Chronicle with the same name=%s already exists", name.c_str());
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
        LOGD("Chronicle name=%s is created", name.c_str());
        return CL_SUCCESS;
    } else {
        LOGE("Fail to create Chronicle name=%s", name.c_str());
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
int ChronicleMetaDirectory::destroy_chronicle(const std::string &name)
{
    LOGD("destroying Chronicle name=%s", name.c_str());
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
        /* Check if Chronicle is acquired by checking if each of its Story is acquired, fail if true */
        Chronicle *pChronicle = chronicleMapRecord->second;
        auto storyMap = pChronicle->getStoryMap();
        int ret = CL_SUCCESS;
        for (auto storyMapRecord: storyMap) {
            Story *pStory = storyMapRecord.second;
            if (!pStory->getAcquirerMap().empty()) {
                ret = CL_ERR_ACQUIRED;
                for (const auto &acquirerMapRecord: pStory->getAcquirerMap()) {
                    LOGD("StoryID=%lu in Chronicle name=%s is still acquired by client_id=%s",
                         pStory->getSid(), name.c_str(), acquirerMapRecord.first.c_str());
                }
            }
        }
        if (ret != CL_SUCCESS) {
            return ret;
        }
        if (pChronicle->getAcquisitionCount() != 0) {
            LOGE("Something is wrong, no Story is being acquired, but Chronicle name=%s's acquisitionCount is not 0",
                 name.c_str());
            return CL_ERR_UNKNOWN;
        }
        /* No Stories in Chronicle is acquired, ready to destroy */
        delete pChronicle;
        auto nErased = chronicleMap_->erase(cid);
//        chronicleName2IdMap_->erase(name);
//        chronicleId2NameMap_->erase(cid);
        t2 = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::nano> duration = (t2 - t1);
        LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        if (nErased == 1) {
            LOGD("Chronicle name=%s is destroyed", name.c_str());
            return CL_SUCCESS;
        } else {
            LOGE("Fail to destroy Chronicle name=%s", name.c_str());
            return CL_ERR_UNKNOWN;
        }
    } else {
        LOGD("Chronicle name=%s does not exist", name.c_str());
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
int ChronicleMetaDirectory::create_story(std::string const& chronicle_name,
                                         const std::string &story_name,
                                         const std::unordered_map<std::string, std::string> &attrs) {
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
        LOGD("Chronicle name=%s does not exist", chronicle_name.c_str());
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
int ChronicleMetaDirectory::destroy_story(std::string const& chronicle_name,
                                          const std::string &story_name
                                          ) {
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
            LOGD("StoryID=%lu name=%s does not exist", sid, story_name.c_str());
            return CL_ERR_NOT_EXIST;
        }
        /* Then check if Story is acquired, fail if true */
        Story *pStory = pChronicle->getStoryMap().at(sid);
        if (!pStory->getAcquirerMap().empty()) {
            for (const auto &acquirerMapRecord: pStory->getAcquirerMap()) {
                LOGD("StoryID=%lu in Chronicle name=%s is still acquired by client_id=%s",
                     pStory->getSid(), chronicle_name.c_str(), acquirerMapRecord.first.c_str());
            }
            return CL_ERR_ACQUIRED;
        }
        /* Ask the Chronicle to destroy the Story */
        CL_Status res = pChronicle->removeStory(chronicle_name, story_name);
        if (res != CL_SUCCESS) {
            LOGE("Fail to remove Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
        }
        return res;
    } else {
        LOGD("Chronicle name=%s does not exist", chronicle_name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Acquire a Story
 * @param client_id: ClientID to acquire the Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @param story_id to populate with the story_id assigned to the story
 * @param notify_keepers , bool value that would be set to true if this is the first client to acquire the story
 * @return CL_SUCCESS if succeed to destroy the Story \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::acquire_story(const std::string &client_id,
                                          const std::string &chronicle_name,
                                          const std::string &story_name,
                                          int &flags
					  , StoryId & story_id, bool& notify_keepers) 
{
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
    if (chronicleMapRecord == chronicleMap_->end()) {
        LOGD("Chronicle name=%s does not exist", chronicle_name.c_str());
        return CL_ERR_NOT_EXIST;
    }
        Chronicle *pChronicle = chronicleMapRecord->second;
        /* Then check if Story already_acquired_by_this_client, fail if false */
        uint64_t sid = pChronicle->getStoryId(story_name);
        if (sid == 0) {
            LOGD("StoryID=%lu name=%s does not exist", sid, story_name.c_str());
	    return CL_ERR_NOT_EXIST;
        }

        /* Last check if this client has acquired this Story already, do nothing and return success if true */
        auto acquirerMap = pChronicle->getStoryMap().at(sid)->getAcquirerMap();
        auto acquirerMapRecord = acquirerMap.find(client_id);
        if (acquirerMapRecord != acquirerMap.end()) {
            LOGD("Story name=%s has already been acquired by client_id=%s", story_name.c_str(), client_id.c_str());
            /* All checks passed, manipulate metadata */
            return CL_ERR_ACQUIRED;
        }

            /* All checks passed, manipulate metadata */
            Story *pStory = pChronicle->getStoryMap().find(sid)->second;
	    story_id = pStory->getSid();
	    notify_keepers = (pStory->getAcquisitionCount() == 0? true :false);

            /* Increment AcquisitionCount */
            pStory->incrementAcquisitionCount();
            /* Add this client to acquirerClientList of the Story */
            pStory->addAcquirerClient(client_id, &clientRegistryManager_->get_client_info(client_id));
            /* Add this Story to acquiredStoryMap for this client */
            clientRegistryManager_->add_story_acquisition(client_id, sid, pStory);
            ret = CL_SUCCESS;
    return ret;
}

/**
 * Release a Story
 * @param client_id: ClientID to release the Story
 * @param chronicle_name: name of the Chronicle that the Story belongs to
 * @param story_name: name of the Story
 * @param flags: flags
 * @param story_id to populate with the story_id assigned to the story
 * @param notify_keepers , bool value that would be set to true if this is the last client to release the story
 * @return CL_SUCCESS if succeed to destroy the Story \n
 *         CL_ERR_NOT_EXIST if the Chronicle does not exist \n
 *         CL_ERR_UNKNOWN otherwise
 */
//TO_DO return acquisition_count after the story has been released
int ChronicleMetaDirectory::release_story(const std::string &client_id,
                                          const std::string &chronicle_name,
                                          const std::string &story_name
					  , StoryId & story_id, bool & notify_keepers ) {
    LOGD("client_id=%s releasing Story name=%s in Chronicle name=%s",
         client_id.c_str(), story_name.c_str(), chronicle_name.c_str());
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
        /* Check if this client actually acquired this Story before, fail if false */
        auto acquirerMap = pChronicle->getStoryMap().at(sid)->getAcquirerMap();
        auto acquirerMapRecord = acquirerMap.find(client_id);
        if (acquirerMapRecord != acquirerMap.end()) {
            /* All checks passed and entry found, manipulate metadata */
            /* Decrement AcquisitionCount */
            pStory->decrementAcquisitionCount();
	    story_id = pStory->getSid();
	    notify_keepers = (pStory->getAcquisitionCount() == 0? true :false);
            /* Remove this client from acquirerClientList of the Story */
            pStory->removeAcquirerClient(client_id);
            /* Remove this Story from acquiredStoryMap for this client */
            ret = clientRegistryManager_->remove_story_acquisition(client_id, sid);
            if (ret != CL_SUCCESS) {
                return ret;
            }
        } else {
            LOGD("Story name=%s is not acquired by client_id=%s, cannot release", story_name.c_str(),
                 client_id.c_str());
            ret = CL_ERR_NOT_ACQUIRED;
        }
    }
    return ret;
}

int ChronicleMetaDirectory::get_chronicle_attr(std::string const& name, const std::string &key, std::string &value) {
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
                LOGD("Property key=%s does not exist in Chronicle name=%s", key.c_str(), name.c_str());
                return CL_ERR_NOT_EXIST;
            }
        } else {
            LOGE("Something is wrong, stored Chronicle object is null");
            return CL_ERR_UNKNOWN;
        }
    } else {
        LOGD("Chronicle name=%s does not exist", name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

int ChronicleMetaDirectory::edit_chronicle_attr(std::string const& name,
                                                const std::string &key,
                                                const std::string &value) {
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
                    LOGE("Something is wrong, fail to insert property key=%s, value=%s", key.c_str(), value.c_str());
                    return CL_ERR_UNKNOWN;
                }
            } else {
                LOGD("Property key=%s does not exist in Chronicle name=%s", key.c_str(), name.c_str());
                return CL_ERR_NOT_EXIST;
            }
        } else {
            LOGE("Something is wrong, stored Chronicle object is null");
            return CL_ERR_UNKNOWN;
        }
    } else {
        LOGD("Chronicle name=%s does not exist", name.c_str());
        return CL_ERR_NOT_EXIST;
    }
}

/**
 * Show all the Chronicles names
 * @param client_id: Client ID
 * @return a vector of the names of all Chronicles
 */
int ChronicleMetaDirectory::show_chronicles( std::vector<std::string>& chronicle_names) 
{
    chronicle_names.clear();

    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    for (auto &[key, value]: *chronicleMap_) {
        chronicle_names.emplace_back(value->getName());
    }
    return CL_SUCCESS;
}

/**
 * Show all the Stories names in a Chronicle
 * @param client_id: Client ID
 * @param chronicle_name: name of the Chronicle
 * @return a vector of the names of given Chronicle \n
 *         empty vector if Chronicle does not exist
 */

int ChronicleMetaDirectory::show_stories(const std::string &chronicle_name, std::vector<std::string> & story_names) 
{
    story_names.clear();

    std::lock_guard<std::mutex> chronicleMapLock(g_chronicleMetaDirectoryMutex_);

    /* First check if Chronicle exists, fail if false */
    
    uint64_t cid;
//    auto name2IdRecord = chronicleName2IdMap_->find(chronicle_name);
//    if (name2IdRecord != chronicleName2IdMap_->end()) {
//        cid = name2IdRecord->second;
    cid = CityHash64(chronicle_name.c_str(), chronicle_name.length());
    auto chronicleMapRecord = chronicleMap_->find(cid);
    if (chronicleMapRecord == chronicleMap_->end()) 
    {   return CL_ERR_NOT_EXIST; }  
 
        Chronicle *pChronicle = chronicleMap_->find(cid)->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        for (auto &[key, value]: pChronicle->getStoryMap()) {
            std::string story_name = value->getName();
            story_names.emplace_back(story_name);
        }
    return CL_SUCCESS;
}
