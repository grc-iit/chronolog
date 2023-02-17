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


ChronicleMetaDirectory::ChronicleMetaDirectory() {
    LOGD("%s constructor is called", typeid(*this).name());
    chronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredChronicleMap_ = new std::unordered_map<uint64_t, Chronicle *>();
    acquiredStoryMap_ = new std::unordered_map<uint64_t, Story *>();
    acquiredChronicleClientMap_ = new std::unordered_multimap<uint64_t,std::string> ();
    acquiredStoryClientMap_ = new std::unordered_multimap<uint64_t,std::string> ();
    LOGD("%s constructor finishes, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());
}

ChronicleMetaDirectory::~ChronicleMetaDirectory() {
    delete chronicleMap_;
    delete acquiredChronicleMap_;
    delete acquiredStoryMap_;
    delete acquiredChronicleClientMap_;
    delete acquiredStoryClientMap_;
}

/**
 * Create a Chronicle
 * @param name: name of the Chronicle
 * @return CL_SUCCESS if succeed to create the Chronicle
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists
 *         CL_ERR_UNKNOWN otherwise
 */
/*int ChronicleMetaDirectory::create_chronicle(const std::string& name) {
    std::unordered_map<std::string, std::string> attrs;
    return create_chronicle(name, attrs);
}*/

/**
 * Create a Chronicle
 * @param name: name of the Chronicle
 * @param attrs: attributes associated with the Chronicle
 * @return CL_SUCCESS if succeed to create the Chronicle
 *         CL_ERR_CHRONICLE_EXISTS if a Chronicle with the same name already exists
 *         CL_ERR_UNKNOWN otherwise
 */
int ChronicleMetaDirectory::create_chronicle(const std::string& name, std::string &client_id, std::string &group_id,
                                             const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(name.c_str(), name.size());
    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    /* Check if Chronicle already exists, fail if true */
    if (chronicleMap_->find(cid) != chronicleMap_->end()) 
    {
	    return CL_ERR_CHRONICLE_EXISTS;
    }
    auto *pChronicle = new Chronicle();
    pChronicle->setName(name);
    pChronicle->setCid(cid);
    pChronicle->add_owner_and_group(client_id,group_id);
    auto attr_iter = attrs.find("Permissions");
    enum ChronoLogVisibility v = CHRONOLOG_RONLY;
    if(attr_iter != attrs.end())
    {
	  std::string c(attr_iter->second);
	  pChronicle->generate_permission(c); 
    }
    else pChronicle->set_permissions(v);
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
					      std::string &client_id,
					      std::string &group_id,
                                              int& flags) {
    LOGD("destroying Chronicle name=%s", name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(name.c_str(), name.size());
    /* Check if Chronicle is acquired, fail if true */
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredChronicleMapLock(g_acquiredChronicleMapMutex_);

    std::string group_id;
    int client_role;

    if (acquiredStoryMap_->find(cid) != acquiredStoryMap_->end())
    {
        return CL_ERR_ACQUIRED;
    }
    /* First check if Chronicle exists, fail if false */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        if (pChronicle->getAcquisitionCount() != 0) {
            return CL_ERR_UNKNOWN;
        }
	enum ChronoLogVisibility v = pChronicle->get_permissions();
	bool b = pChronicle->can_delete_chronicle(client_id,group_id);
	if(b)
        {	
           delete pChronicle;
           auto nErased = chronicleMap_->erase(cid);
           t2 = std::chrono::steady_clock::now();
           std::chrono::duration<double, std::nano> duration = (t2 - t1);
           LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
           if (nErased == 1) 
	   {
            return CL_SUCCESS;
           } 
	   else 
	   {
            return CL_ERR_UNKNOWN;
           }
	}
	else 
	{
	  LOGE("Does not have permissions to delete Chronicle");
	  return CL_ERR_UNKNOWN;
	}

    } else {
        LOGE("Cannot find Chronicle cid=%lu", cid);
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
				              std::string& client_id, std::string &group_id,
                                              int& flags) {
    LOGD("acquiring Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredChronicleMapLock(g_acquiredChronicleMapMutex_);
    /* First check if Chronicle exists, fail if false */
    int ret;
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
	auto range = acquiredChronicleClientMap_->equal_range(cid);
	bool exists = false;
	if( range.first != range.second)
	{
	   for(auto t = range.first; t != range.second; ++t)
	   {
		if(t->second.compare(client_id)==0)
		{
		   exists = true; break;
		}
	   }

	if(!exists)
	{
	   std::pair<uint64_t,std::string> p(cid,client_id);
	   acquiredChronicleClientMap_->insert(p);
	   pChronicle->incrementAcquisitionCount();
	   ret = CL_SUCCESS;
	   if(acquiredChronicleMap_->find(cid) == acquiredChronicleMap_->end())
	   {
	     auto res = acquiredChronicleMap_->emplace(cid,pChronicle);
	     if(res.second) ret = CL_SUCCESS;
	     else ret = CL_ERR_UNKNOWN;
	   }
	}
	else ret = CL_ERR_UNKNOWN;
    }
    else ret = CL_ERR_NOT_EXIST;
    return ret;
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
					      std::string& client_id, std::string &group_id,
                                              int& flags) {
    LOGD("releasing Chronicle name=%s", name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    Chronicle *pChronicle;
    int ret;
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredChronicleMapLock(g_acquiredChronicleMapMutex_);
        /* First check if Chronicle exists */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
     pChronicle = chronicleRecord->second;
     auto range = acquiredChronicleClientMap_->equal_range(cid);
     if(range.first != range.second)
     {
	for(auto t = range.first; t != range.second;)
	{
		if(t->second.compare(client_id)==0) 
		{
		   t = acquiredChronicleClientMap_->erase(t);
		   pChronicle->decrementAcquisitionCount();
		   ret = CL_SUCCESS;
		   if(pChronicle->getAcquisitionCount()==0)
		   {
		      auto nErased = acquiredChronicleMap_->erase(cid);
		      if(nErased==1) ret = CL_SUCCESS;
		      else ret=CL_ERR_UNKNOWN;
		   }
		   break;
		}
	        else ++t;	
	}

     }
     else ret = CL_ERR_UNKNOWN;
     }
     else ret =CL_ERR_NOT_EXIST;	
     return ret;
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
					 std::string &client_id, std::string &group_id,
                                         const std::unordered_map<std::string, std::string>& attrs) {
    LOGD("creating Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::chrono::steady_clock::time_point t1, t2;
    t1 = std::chrono::steady_clock::now();
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) {
        Chronicle *pChronicle = chronicleRecord->second;
        LOGD("Chronicle@%p", &(*pChronicle));
        /* Ask Chronicle to create the Story */
	bool b = pChronicle->can_create_story(client_id,group_id);
	if(b)
	{
          CL_Status res = pChronicle->addStory(chronicle_name, story_name,client_id,group_id,attrs);
          t2 = std::chrono::steady_clock::now();
          std::chrono::duration<double, std::nano> duration = (t2 - t1);
          LOGD("time in %s: %lf ns", __FUNCTION__, duration.count());
        /* Forward its return value */
          return res;
	}
	else 
	{
	   LOGE("Does not have permissions to create story");
	   return CL_ERR_UNKNOWN;
	}
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
					  std::string& client_id, std::string& group_id,
                                          int& flags) {
    LOGD("destroying Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    /* First check if Story is acquired, fail if true */
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredStoryMapLock(g_acquiredStoryMapMutex_);
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());

    if (acquiredStoryMap_->find(sid) != acquiredStoryMap_->end())
    {
        return CL_ERR_ACQUIRED;
    }

    /* Then check if Chronicle exists, fail if false */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
	bool b = pChronicle->can_delete_story(client_id,group_id);
        /* Ask the Chronicle to destroy the Story */
	if(b)
	{
          CL_Status res = pChronicle->removeStory(chronicle_name, story_name,client_id,group_id,flags);
          if (res != CL_SUCCESS) {
            LOGE("Fail to remove Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
        }
	return res;
    }   
    return CL_ERR_NOT_EXIST;
}

int ChronicleMetaDirectory::get_story_list(std::string& chronicle_name, std::vector<std::string>& story_name_list) {
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    Chronicle *pChronicle = chronicleMap_->find(cid)->second;
    for (auto const& story : pChronicle->getStoryMap()) {
        //TODO: need SID to name mapping
        story_name_list.emplace_back(std::to_string(story.first));
    }
    return CL_SUCCESS;
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
int ChronicleMetaDirectory::acquire_story(const std::string &client_id,
					  const std::string& chronicle_name,
                                          const std::string& story_name,
					  std::string& client_id,
					  std::string& group_id,
                                          int& flags) {
    LOGD("acquiring Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    // add cid to name before hash to allow same story name across chronicles
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredStoryMapLock(g_acquiredStoryMapMutex_);
    /* Then check if Chronicle exists, fail if false */
    int ret = CL_ERR_NOT_EXIST;
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
	enum ChronoLogOp op = (enum ChronoLogOp) flags;
	bool b1 = pChronicle->can_acquire_story(client_id,group_id,op);
        auto storyRecord = pChronicle->getStoryMap().find(sid);
        if (storyRecord != pChronicle->getStoryMap().end()) 
	{
            Story *pStory = storyRecord->second;
	    auto range = acquiredStoryClientMap_->equal_range(sid);
	    bool exists = false;
	    if(range.first != range.second)
            /* Increment AcquisitionCount */
	    {
		for(auto t = range.first; t != range.second; ++t)
		{
		   if(t->second.compare(client_id)==0)
		   {
			exists = true; break;
		   }
		}
	    }
	    if(!exists)
	    {
	       pStory->incrementAcquisitionCount();
	       std::pair<uint64_t,std::string> p(sid,client_id);
	       acquiredStoryClientMap_->insert(p);
	       ret = CL_SUCCESS;
	       if(acquiredStoryMap_->find(sid) == acquiredStoryMap_->end())
	       {
		   auto res = acquiredStoryMap_->emplace(sid,pStory);
		   if(res.second) ret = CL_SUCCESS;
		   else ret = CL_ERR_UNKNOWN;
	       }
	    }
	    else ret = CL_ERR_UNKNOWN;
        }
    }
    return ret;
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
int ChronicleMetaDirectory::release_story(const std::string &client_id,
					  const std::string& chronicle_name,
                                          const std::string& story_name,
					  std::string& client_id, std::string& group_id,
                                          int& flags) {
    LOGD("releasing Story name=%s in Chronicle name=%s", story_name.c_str(), chronicle_name.c_str());
    std::string story_name_for_hash = chronicle_name + story_name;
    uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
    uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
    std::lock_guard<std::mutex> ChronicleMapLock(g_chronicleMetaDirectoryMutex_);
    std::lock_guard<std::mutex> AcquiredStoryMapLock(g_acquiredStoryMapMutex_);
    /* Then check if Chronicle exists, fail if false */
    int ret = CL_ERR_NOT_EXIST;
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
        /* Then check if Story exists, fail if false */
        auto storyRecord = pChronicle->getStoryMap().find(sid);
        if (storyRecord != pChronicle->getStoryMap().end()) 
	{
            Story *pStory = storyRecord->second;
            /* Decrement AcquisitionCount */
	    auto range = acquiredStoryClientMap_->equal_range(sid);
	    if(range.first != range.second)
	    {
		for(auto t = range.first; t != range.second;)
		{
		   if(t->second.compare(client_id)==0)
		   {	  
		      t = acquiredStoryClientMap_->erase(t);
		      pStory->decrementAcquisitionCount();
		      ret = CL_SUCCESS;
		      if(pStory->getAcquisitionCount()==0)
		      {
			  auto nErased = acquiredStoryMap_->erase(sid);
			  if(nErased==1) ret = CL_SUCCESS;
			  else ret = CL_ERR_UNKNOWN;
		      }
		      break;
		   }
		   else ++t;
		}
	    }
	    else ret = CL_ERR_UNKNOWN;
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

int ChronicleMetaDirectory::get_chronicle_attr(std::string& name, const std::string& key, std::string& client_id, std::string& group_id, std::string& value) {
    LOGD("getting attributes key=%s from Chronicle name=%s", key.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());
    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
        if (pChronicle) 
	{
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) 
	    {
                value = propertyRecord->second;
                return CL_SUCCESS;
            } else 
                return CL_ERR_NOT_EXIST;
        } 
	else
            return CL_ERR_UNKNOWN;
    } 
    else
        return CL_ERR_NOT_EXIST;
}

int ChronicleMetaDirectory::edit_chronicle_attr(std::string& name,
                                                const std::string& key,
						std::string& client_id, std::string& group_id,
                                                const std::string& value) {
    LOGD("editing attribute key=%s, value=%s from Chronicle name=%s", key.c_str(), value.c_str(), name.c_str());
    uint64_t cid = CityHash64(name.c_str(), name.size());

    std::lock_guard<std::mutex> lock(g_chronicleMetaDirectoryMutex_);
    /* First check if Chronicle exists, fail if false */
    auto chronicleRecord = chronicleMap_->find(cid);
    if (chronicleRecord != chronicleMap_->end()) 
    {
        Chronicle *pChronicle = chronicleRecord->second;
        if (pChronicle) 
	{
            /* Then check if property exists, fail if false */
            auto propertyRecord = pChronicle->getPropertyList().find(key);
            if (propertyRecord != pChronicle->getPropertyList().end()) 
	    {
                auto res = pChronicle->getPropertyList().insert_or_assign(key, value);
                if (res.second) 
                    return CL_SUCCESS;
		else
                  return CL_ERR_UNKNOWN;
            }
	    else 
	    return CL_ERR_NOT_EXIST;
        } 
	else 
           return CL_ERR_UNKNOWN;
       
    } 
    else 
        return CL_ERR_NOT_EXIST;
}
