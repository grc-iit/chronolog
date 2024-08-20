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
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <mutex>

#define MAX_CHRONICLE_PROPERTY_LIST_SIZE 16
#define MAX_CHRONICLE_METADATA_MAP_SIZE 16
#define MAX_STORY_MAP_SIZE 1024
#define MAX_ARCHIVE_MAP_SIZE 1024

enum ChronicleIndexingGranularity
{
    chronicle_gran_ns = 0, chronicle_gran_us = 1, chronicle_gran_ms = 2, chronicle_gran_sec = 3
};

enum ChronicleType
{
    chronicle_type_standard = 0, chronicle_type_priority = 1
};

enum ChronicleTieringPolicy
{
    chronicle_tiering_normal = 0, chronicle_tiering_hot = 1, chronicle_tiering_cold = 2
};

typedef struct ChronicleAttrs_
{
    uint64_t size;
    enum ChronicleIndexingGranularity indexing_granularity;
    enum ChronicleType type;
    enum ChronicleTieringPolicy tiering_policy;
    uint16_t access_permission;
} ChronicleAttrs;

typedef struct ChronicleStats_
{
    uint64_t count;
} ChronicleStats;

class Chronicle
{
public:
    Chronicle()
    {
        propertyList_ = std::unordered_map <std::string, std::string>(MAX_CHRONICLE_PROPERTY_LIST_SIZE);
        metadataMap_ = std::unordered_map <std::string, std::string>(MAX_CHRONICLE_METADATA_MAP_SIZE);
        storyMap_ = std::unordered_map <uint64_t, Story*>(MAX_STORY_MAP_SIZE);
        archiveMap_ = std::unordered_map <uint64_t, Archive*>(MAX_ARCHIVE_MAP_SIZE);
        attrs_.size = 0;
        attrs_.indexing_granularity = chronicle_gran_ms;
        attrs_.type = chronicle_type_standard;
        attrs_.tiering_policy = chronicle_tiering_normal;
        attrs_.access_permission = 0;
        stats_.count = 0;
//        storyName2IdMap_ = new std::unordered_map<std::string, uint64_t>();
//        storyId2NameMap_ = new std::unordered_map<uint64_t, std::string>();
    }

    ~Chronicle()
    {
//        delete storyName2IdMap_;
//        delete storyId2NameMap_;
    }

    void setName(const std::string &name)
    { name_ = name; }

    void setCid(const uint64_t &cid)
    { cid_ = cid; }

    void setStats(const ChronicleStats &stats)
    { stats_ = stats; }

    void setProperty(const std::unordered_map <std::string, std::string> &attrs)
    {
        for(auto const &entry: attrs)
        {
            propertyList_.emplace(entry.first, entry.second);
        }
    }

    const std::string &getName() const
    { return name_; }

    const uint64_t &getCid() const
    { return cid_; }

    const ChronicleStats &getStats() const
    { return stats_; }

    std::unordered_map <std::string, std::string> &getPropertyList()
    { return propertyList_; }

    const std::unordered_map <std::string, std::string> &getMetadataMap() const
    { return metadataMap_; }

    std::unordered_map <uint64_t, Story*> &getStoryMap()
    { return storyMap_; }

    const std::unordered_map <uint64_t, Archive*> &getArchiveMap() const
    { return archiveMap_; }
//    std::unordered_map<std::string, uint64_t> *getName2IdMap() { return storyName2IdMap_; }
//    std::unordered_map<uint64_t, std::string> *getId2NameMap() { return storyId2NameMap_; }

    bool hasStory(const std::string &story_name)
    {
        std::string story_name_for_hash = name_ + story_name;
//        auto name2IdRecord = storyName2IdMap_->find(story_name_for_hash);
//        if (name2IdRecord != storyName2IdMap_->end()) return true;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.length());
        if(storyMap_.find(sid) != storyMap_.end())
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    /**
     * Get StoryID from its name
     * @param story_name: name of the Story
     * @return StoryID if found \n
     *         0 elsewise
     */
    uint64_t getStoryId(const std::string &story_name)
    {
        if(!hasStory(story_name))
        {
            return 0;
        }
        else
        {
            std::string story_name_for_hash = name_ + story_name;
//            return storyName2IdMap_->find(story_name_for_hash)->second;
            return CityHash64(story_name_for_hash.c_str(), story_name_for_hash.length());
        }
    }

    friend std::ostream &operator<<(std::ostream &os, const Chronicle &chronicle);

    int addProperty(const std::string &name, const std::string &value)
    {
        if(propertyList_.size() <= MAX_CHRONICLE_PROPERTY_LIST_SIZE)
        {
            auto res = propertyList_.insert_or_assign(name, value);
            if(res.second)
            {
                return chronolog::CL_SUCCESS;
            }
            else
            {
                return chronolog::CL_ERR_UNKNOWN;
            }
        }
        else
        {
            return chronolog::CL_ERR_CHRONICLE_PROPERTY_FULL;
        }
    }

    int addMetadata(const std::string &name, const std::string &value)
    {
        if(metadataMap_.size() <= MAX_CHRONICLE_METADATA_MAP_SIZE)
        {
            auto res = metadataMap_.insert_or_assign(name, value);
            if(res.second) return chronolog::CL_SUCCESS;
            else return chronolog::CL_ERR_UNKNOWN;
        }
        else
        {
            return chronolog::CL_ERR_CHRONICLE_METADATA_FULL;
        }
    }

    std::pair <int, Story*>
    addStory(const std::string &story_name, const std::map <std::string, std::string> &attrs)
    {
        /* Check if Story exists */
        std::string story_name_for_hash = name_ + story_name;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.length());
        auto story_iter = storyMap_.find(sid);
        if(story_iter != storyMap_.end())
        { return std::pair <int, Story*>(chronolog::CL_SUCCESS, (*story_iter).second); }

        Story*pStory = new Story();
        pStory->setName(story_name);
        pStory->setProperty(attrs);
        pStory->setSid(sid);
        pStory->setCid(cid_);
        LOG_DEBUG("[Chronicle] Adding to StoryMap at {} with {} entries in Chronicle at {}", static_cast<void*>(&storyMap_)
             , storyMap_.size(), static_cast<const void*>(this));
        auto res = storyMap_.emplace(sid, pStory);
//        storyName2IdMap_->insert_or_assign(story_name_for_hash, sid);
//        storyId2NameMap_->insert_or_assign(sid, story_name_for_hash);
        if(res.second)
        { return std::pair <int, Story*>(chronolog::CL_SUCCESS, pStory); }
        else
        { return std::pair <int, Story*>(chronolog::CL_ERR_UNKNOWN, nullptr); }
    }

    int removeStory(std::string const &chronicle_name, const std::string &story_name)
    {
        // add chronicle_name to story_name before hash to allow same story name across chronicles
        std::string story_name_for_hash = chronicle_name + story_name;
        /* Check if Story exists, fail if true */
//        if(storyName2IdMap_->find(story_name_for_hash) != storyName2IdMap_->end()) {
//            uint64_t sid = storyName2IdMap_->find(story_name_for_hash)->second;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.length());
        auto storyMapRecord = storyMap_.find(sid);
        if(storyMapRecord != storyMap_.end())
        {
            Story*pStory = storyMap_.find(sid)->second;
            /* Check if Story is acquired, fail if true */
            if(pStory->getAcquisitionCount() != 0)
            {
                return chronolog::CL_ERR_ACQUIRED;
            }
            delete pStory;
            LOG_DEBUG("[Chronicle] Removing from StoryMap at {} with {} entries in Chronicle at {}"
                 , static_cast<void*>(&storyMap_), storyMap_.size(), static_cast<const void*>(this));
            auto nErased = storyMap_.erase(sid);
//            storyName2IdMap_->erase(story_name_for_hash);
//            storyId2NameMap_->erase(sid);
            if(nErased == 1) return chronolog::CL_SUCCESS;
            else return chronolog::CL_ERR_UNKNOWN;
        }
        return chronolog::CL_ERR_NOT_EXIST;
    }


    int addArchive(uint64_t cid, const std::string &name, const std::unordered_map <std::string, std::string> &attrs)
    {
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        if(archiveMap_.find(aid) != archiveMap_.end()) return false;
        auto*pArchive = new Archive();
        pArchive->setName(name);
        pArchive->setProperty(attrs);
        pArchive->setAid(aid);
        pArchive->setCid(cid);
        LOG_DEBUG("[Chronicle] Adding to ArchiveMap at {} with {} entries in Chronicle at {}"
             , static_cast<void*>(&archiveMap_), archiveMap_.size(), static_cast<const void*>(this));
        auto res = archiveMap_.emplace(aid, pArchive);
        if(res.second) return chronolog::CL_SUCCESS;
        else return chronolog::CL_ERR_UNKNOWN;
    }

    int removeArchive(uint64_t cid, const std::string &name, int flags)
    {
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        auto storyRecord = archiveMap_.find(aid);
        if(storyRecord != archiveMap_.end())
        {
            Archive*pArchive = storyRecord->second;
            delete pArchive;
            LOG_DEBUG("[Chronicle] Removing from ArchiveMap at {} with {} entries in Chronicle at {}"
                 , static_cast<void*>(&archiveMap_), archiveMap_.size(), static_cast<const void*>(this));
            auto nErased = archiveMap_.erase(aid);
            if(nErased == 1) return chronolog::CL_SUCCESS;
            else return chronolog::CL_ERR_UNKNOWN;
        }
        return chronolog::CL_ERR_NOT_EXIST;
    }

    uint64_t incrementAcquisitionCount()
    {
        stats_.count++;
        return stats_.count;
    }

    uint64_t decrementAcquisitionCount()
    {
        stats_.count--;
        return stats_.count;
    }

    uint64_t getAcquisitionCount() const
    { return stats_.count; }

    size_t getPropertyListSize()
    { return propertyList_.size(); }

    size_t getMetadataMapSize()
    { return metadataMap_.size(); }

    size_t getStoryMapSize()
    { return storyMap_.size(); }

    size_t getArchiveMapSize()
    { return archiveMap_.size(); }

private:
    std::string name_;
    uint64_t cid_{};
    ChronicleAttrs attrs_{};
    ChronicleStats stats_{};
    std::unordered_map <std::string, std::string> propertyList_;
    std::unordered_map <std::string, std::string> metadataMap_;
    std::unordered_map <uint64_t, Story*> storyMap_;
    std::unordered_map <uint64_t, Archive*> archiveMap_;
//    std::unordered_map<std::string, uint64_t> *storyName2IdMap_;
//    std::unordered_map<uint64_t, std::string> *storyId2NameMap_;
};

inline std::ostream &operator<<(std::ostream &os, const Chronicle &chronicle)
{
    os << "name: " << chronicle.name_ << ", " << "cid: " << chronicle.cid_ << ", " << "access count: "
       << chronicle.stats_.count << ", " << "properties: ";
    os << "(";
    for(auto const &property: chronicle.propertyList_)
        os << property.first << ": " << property.second << ", ";
    os << ")";
    return os;
}

#endif //CHRONOLOG_CHRONICLE_H
