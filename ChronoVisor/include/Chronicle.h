//
// Created by kfeng on 3/6/22.
//

#ifndef CHRONOLOG_CHRONICLE_H
#define CHRONOLOG_CHRONICLE_H

#include <unordered_map>
#include <ostream>
#include <atomic>
#include <Story.h>
#include <Archive.h>
#include "city.h"
#include <log.h>
#include <errcode.h>
#include <mutex>
#include <enum.h>
#include <algorithm>
#include <iostream>

#define MAX_CHRONICLE_PROPERTY_LIST_SIZE 16
#define MAX_CHRONICLE_METADATA_MAP_SIZE 16
#define MAX_STORY_MAP_SIZE 1024
#define MAX_ARCHIVE_MAP_SIZE 1024

enum ChronicleIndexingGranularity {
    chronicle_gran_ns = 0,
    chronicle_gran_us = 1,
    chronicle_gran_ms = 2,
    chronicle_gran_sec = 3
};

enum ChronicleType {
    chronicle_type_standard = 0,
    chronicle_type_priority = 1
};

enum ChronicleTieringPolicy {
    chronicle_tiering_normal = 0,
    chronicle_tiering_hot = 1,
    chronicle_tiering_cold = 2
};

typedef struct ChronicleAttrs_ {
    uint64_t size;
    enum ChronicleIndexingGranularity indexing_granularity;
    enum ChronicleType type;
    enum ChronicleTieringPolicy tiering_policy;
    enum ChronoLogVisibility access_permission;
    std::vector<std::string> ownerlist;
    std::vector<std::string> grouplist;
} ChronicleAttrs;

typedef struct ChronicleStats_ {
    uint64_t count;
} ChronicleStats;

class Chronicle {
public:
    Chronicle() {
        propertyList_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_PROPERTY_LIST_SIZE);
        metadataMap_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_METADATA_MAP_SIZE);
        storyMap_ = std::unordered_map<uint64_t, Story *>(MAX_STORY_MAP_SIZE);
        archiveMap_ = std::unordered_map<uint64_t, Archive *>(MAX_ARCHIVE_MAP_SIZE);
        attrs_.size = 0;
        attrs_.indexing_granularity = chronicle_gran_ms;
        attrs_.type = chronicle_type_standard;
        attrs_.tiering_policy = chronicle_tiering_normal;
        stats_.count = 0;
    }

    void setName(const std::string &name) { name_ = name; }
    void setCid(const uint64_t &cid) { cid_ = cid; }
    void setStats(const ChronicleStats &stats) { stats_ = stats; }
    void setProperty(const std::unordered_map<std::string, std::string>& attrs) {
        for (auto const& entry : attrs) {
            propertyList_.emplace(entry.first, entry.second);
        }
    }

    const std::string &getName() const { return name_; }
    const uint64_t &getCid() const { return cid_; }
    const ChronicleStats &getStats() const { return stats_; }
    std::unordered_map<std::string, std::string> &getPropertyList() { return propertyList_; }
    const std::unordered_map<std::string, std::string> &getMetadataMap() const { return metadataMap_; }
    std::unordered_map<uint64_t, Story *> &getStoryMap() { return storyMap_; }
    const std::unordered_map<uint64_t, Archive *> &getArchiveMap() const { return archiveMap_; }

    friend std::ostream& operator<<(std::ostream& os, const Chronicle& chronicle);

    int addProperty(const std::string& name, const std::string& value) {
        if (propertyList_.size() <= MAX_CHRONICLE_PROPERTY_LIST_SIZE) {
            auto res = propertyList_.insert_or_assign(name, value);
            if (res.second) return CL_SUCCESS;
            else return CL_ERR_UNKNOWN;
        } else {
            return CL_ERR_CHRONICLE_PROPERTY_FULL;
        }
    }

    int addMetadata(const std::string& name, const std::string& value) {
        if (metadataMap_.size() <= MAX_CHRONICLE_METADATA_MAP_SIZE) {
            auto res = metadataMap_.insert_or_assign(name, value);
            if (res.second) return CL_SUCCESS;
            else return CL_ERR_UNKNOWN;
        } else {
            return CL_ERR_CHRONICLE_METADATA_FULL;
        }
    }

    int addStory(std::string &chronicle_name, const std::string& story_name, std::string &client_id, std::string &group_id,
                 const std::unordered_map<std::string, std::string>& attrs) {
        // add cid to name before hash to allow same story name across chronicles
        std::string story_name_for_hash = chronicle_name + story_name;
        uint64_t cid = CityHash64(chronicle_name.c_str(), chronicle_name.size());
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
        if(storyMap_.find(sid) != storyMap_.end()) return CL_ERR_STORY_EXISTS;
        auto *pStory = new Story();
        pStory->setName(story_name);
        pStory->setProperty(attrs);
        pStory->setSid(sid);
        pStory->setCid(cid);
	pStory->add_owner_and_group(client_id,group_id);
	enum ChronoLogVisibility v=CHRONOLOG_RONLY;
	auto attr_iter = attrs.find("Permissions");
	if(attr_iter != attrs.end())
	{
	   std::string c(attr_iter->second);
	   pStory->generate_permission(c);
	}
	else pStory->set_permissions(v);
        LOGD("adding to storyMap@%p with %lu entries in Chronicle@%p",
             &storyMap_, storyMap_.size(), this);
        auto res = storyMap_.emplace(sid, pStory);
        if (res.second) return CL_SUCCESS;
        else return CL_ERR_UNKNOWN;
    }

    int removeStory(std::string &chronicle_name, const std::string& story_name,std::string client_id,std::string group_id,int flags) {
        // add cid to name before hash to allow same story name across chronicles
        std::string story_name_for_hash = chronicle_name + story_name;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
        auto storyRecord = storyMap_.find(sid);
        if (storyRecord != storyMap_.end()) {
            Story *pStory = storyRecord->second;
            if (pStory->getAcquisitionCount() != 0) {
               return CL_ERR_ACQUIRED;
            }
             
	    bool b = pStory->can_delete_story(client_id,group_id);
	    if(b)
	    { 
	      delete pStory;
              LOGD("removing from storyMap@%p with %lu entries in Chronicle@%p",
                 &storyMap_, storyMap_.size(), this);
              auto nErased = storyMap_.erase(sid);
              if (nErased == 1) return CL_SUCCESS;
              else return CL_ERR_UNKNOWN;
	    }
	    else
	    {
	       LOGD("Doesnot have permission to delete story");
	       return CL_ERR_UNKNOWN;
	    }
        }
        return CL_ERR_NOT_EXIST;
    }


    int addArchive(uint64_t cid, const std::string& name, const std::unordered_map<std::string, std::string>& attrs) {
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        if (archiveMap_.find(aid) != archiveMap_.end()) return false;
        auto *pArchive = new Archive();
        pArchive->setName(name);
        pArchive->setProperty(attrs);
        pArchive->setAid(aid);
        pArchive->setCid(cid);
        LOGD("adding to archiveMap@%p with %lu entries in Chronicle@%p",
             &archiveMap_, archiveMap_.size(), this);
        auto res = archiveMap_.emplace(aid, pArchive);
        if (res.second) return CL_SUCCESS;
        else return CL_ERR_UNKNOWN;
    }

    int removeArchive(uint64_t cid, const std::string& name, int flags) {
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        auto storyRecord = archiveMap_.find(aid);
        if (storyRecord != archiveMap_.end()) {
            Archive *pArchive = storyRecord->second;
            delete pArchive;
            LOGD("removing from archiveMap@%p with %lu entries in Chronicle@%p",
                 &archiveMap_, archiveMap_.size(), this);
            auto nErased = archiveMap_.erase(aid);
            if (nErased == 1) return CL_SUCCESS;
            else return CL_ERR_UNKNOWN;
        }
        return CL_ERR_NOT_EXIST;
    }

    uint64_t incrementAcquisitionCount() {
        stats_.count++;
        return stats_.count;
    }
    uint64_t decrementAcquisitionCount() {
        stats_.count--;
        return stats_.count;
    }
    uint64_t getAcquisitionCount() const { return stats_.count; }

    size_t getPropertyListSize() { return propertyList_.size(); }
    size_t getMetadataMapSize() { return metadataMap_.size(); }
    size_t getStoryMapSize() { return storyMap_.size(); }
    size_t getArchiveMapSize() { return archiveMap_.size(); }

    inline int set_permissions(enum ChronoLogVisibility &v)
    {
	    attrs_.access_permission = v;
	    return CL_SUCCESS;
    }
    inline enum ChronoLogVisibility & get_permissions()
    {
	  return attrs_.access_permission;
    }
    inline void generate_permission(std::string &perm)
    {
	if(perm.compare("RWCD")==0) attrs_.access_permission = CHRONOLOG_RWCD;
        else if(perm.compare("RWC")==0) attrs_.access_permission = CHRONOLOG_RWC;
	else if(perm.compare("RWD")==0) attrs_.access_permission = CHRONOLOG_RWD;
	else if(perm.compare("RW")==0) attrs_.access_permission = CHRONOLOG_RW;
	else if(perm.compare("RO")==0) attrs_.access_permission = CHRONOLOG_RONLY;	
	else attrs_.access_permission = CHRONOLOG_RONLY;
    }
    inline bool can_create_story(std::string &client_id, std::string &group_id)
    {
	if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
	  if(attrs_.access_permission == CHRONOLOG_RWC || attrs_.access_permission == CHRONOLOG_RWCD) return true;
	}
	return false;
    }
    inline bool can_acquire_story(std::string &client_id,std::string &group_id,enum ChronoLogOp &op)
    {
	if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
	   if(op == CHRONOLOG_READ && (attrs_.access_permission == CHRONOLOG_RONLY || attrs_.access_permission == CHRONOLOG_RW ||
	    attrs_.access_permission == CHRONOLOG_RWC || attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD))
		   return true;
	   else if(op == CHRONOLOG_WRITE && (attrs_.access_permission == CHRONOLOG_RW || attrs_.access_permission == CHRONOLOG_RWC || attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD))
		   return true;
	}
	return false;

    }
    inline bool can_delete_story(std::string &client_id, std::string &group_id)
    {
	 if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	    std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	 {
	   if(attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD) return true;
	 }
	 return false;
    }
    inline bool can_delete_chronicle(std::string &client_id, std::string &group_id)
    {
	if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
           if(attrs_.access_permission == CHRONOLOG_RWCD || attrs_.access_permission == CHRONOLOG_RWD) return true;	
	}
	return false;
    }
    inline bool can_acquire_chronicle(std::string &client_id, std::string &group_id,enum ChronoLogOp &op)
    {
        if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
	   if(op == CHRONOLOG_READ && (attrs_.access_permission == CHRONOLOG_RONLY ||
	    attrs_.access_permission == CHRONOLOG_RW || attrs_.access_permission == CHRONOLOG_RWC || attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD))
	      return true;		   
	   else if(op == CHRONOLOG_WRITE && (attrs_.access_permission == CHRONOLOG_RW || attrs_.access_permission == CHRONOLOG_RWC || attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD))
	      return true;
	   else if(op == CHRONOLOG_FILEOP && attrs_.access_permission == CHRONOLOG_RWCD)
		   return true;
	}

	return false;
    }
    inline int add_owner_and_group(std::string &client_id, std::string &group_id)
    {
	attrs_.ownerlist.emplace(attrs_.ownerlist.end(),client_id);
	attrs_.grouplist.emplace(attrs_.grouplist.end(),group_id);
        return CL_SUCCESS;	
    }
    inline int get_owner_and_group(std::vector<std::string> &owner, std::vector<std::string> &group)
    {
        owner.assign(attrs_.ownerlist.begin(),attrs_.ownerlist.end());
        group.assign(attrs_.grouplist.begin(),attrs_.grouplist.end());
        return CL_SUCCESS;
    }	

private:
    std::string name_;
    uint64_t cid_{};
    ChronicleAttrs attrs_{};
    ChronicleStats stats_{};
    std::unordered_map<std::string, std::string> propertyList_;
    std::unordered_map<std::string, std::string> metadataMap_;
    std::unordered_map<uint64_t, Story *> storyMap_;
    std::unordered_map<uint64_t, Archive *> archiveMap_;
};

inline std::ostream& operator<<(std::ostream& os, const Chronicle& chronicle) {
    os << "name: " << chronicle.name_ << ", "
       << "cid: " << chronicle.cid_ << ", "
       << "access count: " << chronicle.stats_.count << ", "
       << "properties: ";
    os << "(";
    for (auto const& property : chronicle.propertyList_)
        os << property.first << ": " << property.second << ", ";
    os << ")";
    return os;
}

#endif //CHRONOLOG_CHRONICLE_H
