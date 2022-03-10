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

#define MAX_CHRONICLE_PROPERTY_LIST_SIZE 16
#define MAX_CHRONICLE_METADATA_MAP_SIZE 16

typedef struct ChronicleStats_ {
    uint64_t count;
} ChronicleStats;

class Chronicle {
public:
    Chronicle() {
        propertyList_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_PROPERTY_LIST_SIZE);
        metadataMap_ = std::unordered_map<std::string, std::string>(MAX_CHRONICLE_METADATA_MAP_SIZE);
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
            propertyList_.emplace(name, value);
            return true;
        } else {
            return false;
        }
    }

    bool addMetadata(const std::string& name, const std::string& value) {
        if (metadataMap_.size() <= MAX_CHRONICLE_METADATA_MAP_SIZE) {
            metadataMap_.emplace(name, value);
            return true;
        } else {
            return false;
        }
    }



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
