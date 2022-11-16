//
// Created by kfeng on 3/9/22.
//

#ifndef CHRONOLOG_CHRONICLEMETADIRECTORY_H
#define CHRONOLOG_CHRONICLEMETADIRECTORY_H

#include <string>
#include <unordered_map>
#include <vector>
#include <Chronicle.h>
#include <memory>

class ChronicleMetaDirectory {
public:
    ChronicleMetaDirectory();
    ~ChronicleMetaDirectory();

//    std::shared_ptr<std::unordered_map<std::string, Chronicle *>> getChronicleMap() { return chronicleMap_; }
    std::unordered_map<std::string, Chronicle *> *getChronicleMap() { return chronicleMap_; }

    bool create_chronicle(const std::string& name);
    bool create_chronicle(const std::string& name, const std::unordered_map<std::string, std::string>& attrs);
    bool destroy_chronicle(const std::string& name, int flags);
    uint64_t acquire_chronicle(const std::string& name, int flags);
    bool release_chronicle(uint64_t cid, int flags);

    bool create_story(uint64_t cid, const std::string& name, const std::unordered_map<std::string, std::string>& attrs);
    bool destroy_story(uint64_t cid, const std::string& name, int flags);
    std::vector<std::string> get_story_list(uint64_t cid);
    uint64_t acquire_story(uint64_t cid, const std::string& name, int flags);
    bool release_story(uint64_t sid, int flags);

    uint64_t record_event(uint64_t sid, void *data);
    uint64_t playback_event(uint64_t sid);

    std::string get_chronicle_attr(uint64_t &cid, const std::string& key);
    bool edit_chronicle_attr(uint64_t &cid, const std::string& key, std::string value);

private:
//    std::shared_ptr<std::unordered_map<std::string, Chronicle *>> chronicleMap_;
    std::unordered_map<std::string, Chronicle *> *chronicleMap_;
};

#endif //CHRONOLOG_CHRONICLEMETADIRECTORY_H
