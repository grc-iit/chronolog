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
#include <city.h>
#include <log.h>

#define MAX_CHRONICLE_PROPERTY_LIST_SIZE 16
#define MAX_CHRONICLE_METADATA_MAP_SIZE 16
#define MAX_STORY_MAP_SIZE 1024
#define MAX_ARCHIVE_MAP_SIZE 1024

typedef struct ChronicleStats_ {
    uint64_t count;
} ChronicleStats;

class Chronicle {
public:
    Chronicle() {
        propertyList_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_PROPERTY_LIST_SIZE);
        metadataMap_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_METADATA_MAP_SIZE);
        storyMap_ = std::unordered_map<std::string, Story *>(MAX_STORY_MAP_SIZE);
        archiveMap_ = std::unordered_map<std::string, Archive *>(MAX_ARCHIVE_MAP_SIZE);
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
    std::unordered_map<std::string, Story *> &getStoryMap() { return storyMap_; }
    const std::unordered_map<std::string, Archive *> &getArchiveMap() const { return archiveMap_; }

    friend std::ostream& operator<<(std::ostream& os, const Chronicle& chronicle);

    bool addProperty(const std::string& name, const std::string& value) {
        if (propertyList_.size() <= MAX_CHRONICLE_PROPERTY_LIST_SIZE) {
            auto res = propertyList_.emplace(name, value);
            return res.second;
        } else {
            return false;
        }
    }

    bool addMetadata(const std::string& name, const std::string& value) {
        if (metadataMap_.size() <= MAX_CHRONICLE_METADATA_MAP_SIZE) {
            auto res = metadataMap_.emplace(name, value);
            return res.second;
        } else {
            return false;
        }
    }

    bool addStory(uint64_t cid, const std::string& name, const std::unordered_map<std::string, std::string>& attrs) {
        Story *pStory = new Story();
        pStory->setName(name);
        // add cid to name before hash to allow same story name across chronicles
        std::string story_name_for_hash = std::to_string(cid) + name;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
        pStory->setSid(sid);
        pStory->setCid(cid);
        LOGD("adding to storyMap at address %lu with %d entries in Chronicle at address %lu",
             &storyMap_, storyMap_.size(), this);
        auto res = storyMap_.emplace(std::to_string(sid), pStory);
        return res.second;
    }

    bool removeStory(uint64_t cid, const std::string& name, int flags) {
        // add cid to name before hash to allow same story name across chronicles
        std::string story_name_for_hash = std::to_string(cid) + name;
        uint64_t sid = CityHash64(story_name_for_hash.c_str(), story_name_for_hash.size());
        auto storyRecord = storyMap_.find(std::to_string(sid));
        if (storyRecord != storyMap_.end()) {
            Story *pStory = storyRecord->second;
            delete pStory;
            LOGD("removing from storyMap at address %lu with %d entries in Chronicle at address %lu",
                 &storyMap_, storyMap_.size(), this);
            auto nErased = storyMap_.erase(std::to_string(sid));
            return (nErased == 1);
        }
        return false;
    }


    bool addArchive(uint64_t cid, const std::string& name, const std::unordered_map<std::string, std::string>& attrs) {
        Archive *pArchive = new Archive();
        pArchive->setName(name);
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        pArchive->setAid(aid);
        pArchive->setCid(cid);
        LOGD("adding to archiveMap at address %lu with %d entries in Chronicle at address %lu",
             &archiveMap_, archiveMap_.size(), this);
        auto res = archiveMap_.emplace(std::to_string(aid), pArchive);
        return res.second;
    }

    bool removeArchive(uint64_t cid, const std::string& name, int flags) {
        // add cid to name before hash to allow same archive name across chronicles
        std::string archive_name_for_hash = std::to_string(cid) + name;
        uint64_t aid = CityHash64(archive_name_for_hash.c_str(), archive_name_for_hash.size());
        auto storyRecord = archiveMap_.find(std::to_string(aid));
        if (storyRecord != archiveMap_.end()) {
            Archive *pArchive = storyRecord->second;
            delete pArchive;
            LOGD("removing from archiveMap at address %lu with %d entries in Chronicle at address %lu",
                 &archiveMap_, archiveMap_.size(), this);
            auto nErased = archiveMap_.erase(std::to_string(aid));
            return (nErased == 1);
        }
        return false;
    }

    size_t getPropertyListSize() { return propertyList_.size(); }
    size_t getMetadataMapSize() { return metadataMap_.size(); }
    size_t getStoryMapSize() { return storyMap_.size(); }
    size_t getArchiveMapSize() { return archiveMap_.size(); }

private:
    std::string name_;
    uint64_t cid_{};
    ChronicleStats stats_{};
    std::unordered_map<std::string, std::string> propertyList_;
    std::unordered_map<std::string, std::string> metadataMap_;
    std::unordered_map<std::string, Story *> storyMap_;
    std::unordered_map<std::string, Archive *> archiveMap_;
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
