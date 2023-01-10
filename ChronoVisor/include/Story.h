//
// Created by kfeng on 3/6/22.
//

#ifndef CHRONOLOG_STORY_H
#define CHRONOLOG_STORY_H

#include <unordered_map>
#include <atomic>
#include <Event.h>
#include "city.h"
#include <mutex>

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
    uint16_t access_permission;
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
        attrs_.access_permission = 0;
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
    void setStats(const StoryStats &stats) { std::lock_guard<std::mutex> lock(statsMutex_); stats_ = stats; }
    void setProperty(const std::unordered_map<std::string, std::string>& attrs) {
        for (auto const& entry : attrs) {
            propertyList_.emplace(entry.first, entry.second);
        }
    }
    void setEventMap(const std::unordered_map<std::string, Event> &eventMap) { eventMap_ = eventMap; }

    uint64_t incrementAcquisitionCount() {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.count++;
        return stats_.count;
    }
    uint64_t decrementAcquisitionCount() {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.count--;
        return stats_.count;
    }
    uint64_t getAcquisitionCount() const { return stats_.count; }

    friend std::ostream& operator<<(std::ostream& os, const Story& story);

    std::mutex statsMutex_;

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
