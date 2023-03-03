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
#include <unordered_map>
#include "city.h"

class ChronicleMetaDirectory {
public:
    ChronicleMetaDirectory();
    ~ChronicleMetaDirectory();

    void set_acl_db(ACL_DB *ad) 
    {
	    use_acl_db = true;
	    adb = ad;
    }
//    std::shared_ptr<std::unordered_map<std::string, Chronicle *>> getChronicleMap() { return chronicleMap_; }
    std::unordered_map<std::string, Chronicle *,stringhashfn> *getChronicleMap() { return chronicleMap_; }

    //int create_chronicle(const std::string& name, std::string &client_id, std::string &group_id,enum ChronoLogVisibility &v);
    int create_chronicle(const std::string& name,std::string &client_id, std::string &group_id, const std::unordered_map<std::string, std::string>& attrs);
    int destroy_chronicle(const std::string& name, std::string &client_id, std::string &group_id, int& flags);
    int acquire_chronicle(const std::string& name, std::string &client_id, std::string &group_id, int& flags);
    int release_chronicle(const std::string& name, std::string &client_id, std::string &group_id, int& flags);
    int update_chronicle_permissions(std::string &name,std::string &client_id,std::string &group_id,std::string &perm);
    int create_story(std::string &chronicle_name, const std::string& story_name, std::string &client_id, std::string &group_id, const std::unordered_map<std::string, std::string>& attrs);
    int destroy_story(std::string &chronicle_name, const std::string& story_name, std::string &client_id, std::string &group_id, int& flags);
    int get_story_list(std::string &chronicle_name, std::vector<std::string> &story_name_list);
    int acquire_story(const std::string& chronicle_name, const std::string& story_name, std::string &client_id, std::string &group_id, int& flags);
    int release_story(const std::string& chronicle_name, const std::string& story_name, std::string &client_id, std::string &group_id, int& flags);
    int update_story_permissions(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &group_id,std::string &perm);
    uint64_t record_event(uint64_t sid, void *data);
    uint64_t playback_event(uint64_t sid);
    int get_chronicle_attr(std::string& name, const std::string& key, std::string &client_id, std::string &group_id, std::string& value);
    int edit_chronicle_attr(std::string& name, const std::string& key, std::string &client_id, std::string &group_id, const std::string& value);
    int add_group_to_chronicle(std::string &name,std::string &client_id,std::string &group_id,std::string &new_group_id,std::string &new_perm);
    int add_group_to_story(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &group_id,std::string &new_group_id,std::string &new_perm);
    int remove_group_from_chronicle(std::string &name,std::string &client_id,std::string &group_id,std::string &new_group_id);
    int remove_group_from_story(std::string &chronicle_name,std::string &story_name,std::string &client_id,std::string &group_id,std::string &new_group_id);
private:
//    std::shared_ptr<std::unordered_map<std::string, Chronicle *>> chronicleMap_;
    //ACL_DB *acl_repo;
    std::unordered_map<std::string , Chronicle *,stringhashfn> *chronicleMap_;
    std::unordered_map<std::string, Chronicle *,stringhashfn> *acquiredChronicleMap_;
    std::unordered_map<std::string, Story *,stringhashfn> *acquiredStoryMap_;
    std::unordered_multimap<std::string, std::string,stringhashfn> *acquiredChronicleClientMap_;
    std::unordered_multimap<std::string, std::string,stringhashfn> *acquiredStoryClientMap_;
    std::mutex g_chronicleMetaDirectoryMutex_;      
    std::mutex g_acquiredChronicleMapMutex_;         
    std::mutex g_acquiredStoryMapMutex_;   
    bool use_acl_db;   
    ACL_DB *adb; 
};

#endif //CHRONOLOG_CHRONICLEMETADIRECTORY_H
