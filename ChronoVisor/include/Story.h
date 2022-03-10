//
// Created by kfeng on 3/6/22.
//

#ifndef CHRONOLOG_STORY_H
#define CHRONOLOG_STORY_H

#include <unordered_map>
#include <atomic>
#include <Event.h>
#include <city.h>

typedef struct StoryStats_ {
    uint64_t count;
} StoryStats;

class Story {
public:
    Story() {}

    const std::string &getName() const { return name_; }
    uint64_t getSid() const { return sid_; }
    uint64_t getCid() const { return cid_; }
    const StoryStats &getStats() const { return stats_; }
    const std::unordered_map<std::string, Event> &getEventMap() const { return eventMap_; }

    void setName(const std::string &name) { name_ = name; }
    void setSid(uint64_t sid) { sid_ = sid; }
    void setCid(uint64_t cid) { cid_ = cid; }
    void setStats(const StoryStats &stats) { stats_ = stats; }
    void setEventMap(const std::unordered_map<std::string, Event> &eventMap) { eventMap_ = eventMap; }

    friend std::ostream& operator<<(std::ostream& os, const Story& story);

private:
    std::string name_;
    uint64_t sid_{};
    uint64_t cid_{};
    StoryStats stats_{};
    std::unordered_map<std::string, Event> eventMap_;
};

inline std::ostream& operator<<(std::ostream& os, const Story& story) {
    os << "name: " << story.name_ << ", "
       << "sid: " << story.sid_ << ", "
       << "access count: " << story.stats_.count;
    return os;
}

#endif //CHRONOLOG_STORY_H
