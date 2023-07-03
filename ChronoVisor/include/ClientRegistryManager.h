//
// Created by kfeng on 7/11/22.
//

#ifndef CHRONOLOG_CLIENTREGISTRYMANAGER_H
#define CHRONOLOG_CLIENTREGISTRYMANAGER_H

#include <unordered_map>
#include <vector>
#include <memory>
#include <ClientInfo.h>
#include <errcode.h>

class ChronicleMetaDirectory;

class ClientRegistryManager {
public:
    ClientRegistryManager();
    ~ClientRegistryManager();

    void setChronicleMetaDirectory(ChronicleMetaDirectory *pChronicleMetaDirectory) {
        chronicleMetaDirectory_ = pChronicleMetaDirectory;
    }

    ClientInfo& get_client_info(const std::string &client_id);
    int add_story_acquisition(const std::string &client_id, uint64_t &sid, Story *pStory);
    int remove_story_acquisition(const std::string &client_id, uint64_t &sid);

    int add_client_record(const std::string &client_id, const ClientInfo &record);
    int remove_client_record(const std::string& client_id);
private:
    std::unordered_map<std::string, ClientInfo> *clientRegistry_;
    std::mutex g_clientRegistryMutex_;
    ChronicleMetaDirectory *chronicleMetaDirectory_{};
};

#endif //CHRONOLOG_CLIENTREGISTRYMANAGER_H
