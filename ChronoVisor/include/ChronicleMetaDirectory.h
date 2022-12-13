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
    std::unordered_map<uint64_t, Chronicle *> *getChronicleMap() { return chronicleMap_; }

    bool create_chronicle(const std::string& name);
    bool create_chronicle(const std::string& name, const std::unordered_map<std::string, std::string>& attrs);
    bool destroy_chronicle(const std::string& name, int flags);
    uint64_t acquire_chronicle(const std::string& name, int flags);
    bool release_chronicle(const std::string& name, int flags);

    bool create_story(std::string &chronicle_name, const std::string& story_name, const std::unordered_map<std::string, std::string>& attrs);
    bool destroy_story(std::string &chronicle_name, const std::string& story_name, int flags);
    std::vector<uint64_t> get_story_list(std::string &chronicle_name);
    uint64_t acquire_story(const std::string &chronicle_name, const std::string& story_name, int flags);
    bool release_story(const std::string &chronicle_name, const std::string &story_name, int flags);

    uint64_t record_event(uint64_t sid, void *data);
    uint64_t playback_event(uint64_t sid);

    std::string get_chronicle_attr(std::string &name, const std::string& key);
    bool edit_chronicle_attr(std::string &name, const std::string& key, std::string value);

private:
//    std::shared_ptr<std::unordered_map<std::string, Chronicle *>> chronicleMap_;
    std::unordered_map<uint64_t , Chronicle *> *chronicleMap_;
    std::unordered_map<uint64_t, Chronicle *> *acquiredChronicleMap_;
    std::unordered_map<uint64_t, Story *> *acquiredStoryMap_;
};

#endif //CHRONOLOG_CHRONICLEMETADIRECTORY_H
