//
// Created by kfeng on 3/6/22.
//

#ifndef CHRONOLOG_STORY_H
#define CHRONOLOG_STORY_H

#include <unordered_map>
#include <atomic>
#include <Event.h>
#include <mutex>
#include <city.h>
#include <errcode.h>
#include <enum.h>
#include <algorithm>
#include <iostream>

enum StoryIndexingGranularity {
    story_gran_ns = 0,
    story_gran_us = 1,
    story_gran_ms = 2,
    story_gran_sec = 3
};

enum StoryType {
    story_type_standard = 0,
    story_type_priority = 1
};

enum StoryTieringPolicy {
    story_tiering_normal = 0,
    story_tiering_hot = 1,
    story_tiering_cold = 2
};

typedef struct StoryAttrs_ {
    uint64_t size;
    enum StoryIndexingGranularity indexing_granularity;
    enum StoryType type;
    enum StoryTieringPolicy tiering_policy;
    enum ChronoLogVisibility access_permission;
    std::vector<std::string> ownerlist;
    std::vector<std::string> grouplist;
} StoryAttrs;

typedef struct StoryStats_ {
    uint64_t count;
} StoryStats;

class Story {
public:
    Story() {
        attrs_.size = 0;
        attrs_.indexing_granularity = story_gran_ms;
        attrs_.type = story_type_standard;
        attrs_.tiering_policy = story_tiering_normal;
        stats_.count = 0;
    }

    const std::string &getName() const { return name_; }
    uint64_t getSid() const { return sid_; }
    uint64_t getCid() const { return cid_; }
    const StoryStats &getStats() const { return stats_; }
    const std::unordered_map<std::string, std::string> &getProperty() const { return propertyList_; }
    const std::unordered_map<std::string, Event> &getEventMap() const { return eventMap_; }

    void setName(const std::string &name) { name_ = name; }
    void setSid(uint64_t sid) { sid_ = sid; }
    void setCid(uint64_t cid) { cid_ = cid; }
    void setStats(const StoryStats &stats) { stats_ = stats; }
    void setProperty(const std::unordered_map<std::string, std::string>& attrs) {
        for (auto const& entry : attrs) {
            propertyList_.emplace(entry.first, entry.second);
        }
    }
    void setEventMap(const std::unordered_map<std::string, Event> &eventMap) { eventMap_ = eventMap; }

    uint64_t incrementAcquisitionCount() {
        stats_.count++;
        return stats_.count;
    }
    uint64_t decrementAcquisitionCount() {
        stats_.count--;
        return stats_.count;
    }
    uint64_t getAcquisitionCount() const { return stats_.count; }
    inline int add_owner_and_group(std::string &client_id, std::string &group_id)
    {
	 attrs_.ownerlist.emplace(attrs_.ownerlist.end(),client_id);
	 attrs_.grouplist.emplace(attrs_.grouplist.end(),group_id);
	 return CL_SUCCESS;
    }
   inline int add_group(std::string &group_id)
   {
	if(std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id)==attrs_.grouplist.end())
	       attrs_.grouplist.emplace(attrs_.grouplist.end(),group_id);
	return CL_SUCCESS;
    }
    inline int remove_group(std::string &group_id)
    {
	 auto iter = std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id);
         if(iter != attrs_.grouplist.end())
	 {
	    attrs_.grouplist.erase(iter);
	    return CL_SUCCESS;
         }
	 return CL_ERR_NOT_EXIST;
    }	 
    inline enum ChronoLogVisibility& get_permissions()
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
    inline int set_permissions(enum ChronoLogVisibility &v)
    {
	 attrs_.access_permission = v;
	 return CL_SUCCESS;
    }
    inline int get_owner_and_group(std::vector<std::string> &owner, std::vector<std::string> &group)
    {
	  owner.assign(attrs_.ownerlist.begin(),attrs_.ownerlist.end());
	  group.assign(attrs_.grouplist.begin(),attrs_.grouplist.end());
	  return CL_SUCCESS;
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
    inline bool can_delete_story(std::string &client_id,std::string &group_id)
    {
	if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
	    if(attrs_.access_permission == CHRONOLOG_RWD || attrs_.access_permission == CHRONOLOG_RWCD) return true;
	}
	return false;
    }
    inline bool can_edit_group(std::string &client_id,std::string &group_id)
    {
	if(std::find(attrs_.ownerlist.begin(),attrs_.ownerlist.end(),client_id) != attrs_.ownerlist.end() ||
	   std::find(attrs_.grouplist.begin(),attrs_.grouplist.end(),group_id) != attrs_.grouplist.end())
	{
	     return true;
	}
	return false;

    }

    friend std::ostream& operator<<(std::ostream& os, const Story& story);

private:
    std::string name_;
    uint64_t sid_{};
    uint64_t cid_{};
    StoryAttrs attrs_{};
    StoryStats stats_{};
    std::unordered_map<std::string, std::string> propertyList_;
    std::unordered_map<std::string, Event> eventMap_;
};

inline std::ostream& operator<<(std::ostream& os, const Story& story) {
    os << "name: " << story.name_ << ", "
       << "sid: " << story.sid_ << ", "
       << "access count: " << story.stats_.count;
    return os;
}

#endif //CHRONOLOG_STORY_H
