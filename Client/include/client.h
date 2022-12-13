//
// Created by kfeng on 5/17/22.
//

#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include "ChronicleMetadataRPCProxy.h"
#include "ChronoLogAdminRPCProxy.h"

class ChronoLogClient {
public:
    ChronoLogClient() { CHRONOLOG_CONF->ConfigureDefaultClient("../../test/communication/server_list"); }

    ChronoLogClient(const std::string& server_list_file_path) {
        CHRONOLOG_CONF->ConfigureDefaultClient(server_list_file_path);
        metadataRpcProxy_ = ChronoLog::Singleton<ChronicleMetadataRPCProxy>::GetInstance();
        adminRpcProxy_ = ChronoLog::Singleton<ChronoLogAdminRPCProxy>::GetInstance();
    }

    bool Connect(const std::string &server_uri, std::string &client_id);
    bool Disconnect(const std::string &client_id, int &flags);
    bool CreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs);
    bool DestroyChronicle(std::string &name, const int &flags);
    bool AcquireChronicle(std::string &name, const int &flags);
    bool ReleaseChronicle(std::string &name, const int &flags);
    bool CreateStory(std::string &chronicle_name, std::string &story_name,
                     const std::unordered_map<std::string, std::string> &attrs);
    bool DestroyStory(std::string &chronicle_name, std::string &story_name, const int &flags);
    bool AcquireStory(std::string &chronicle_name, std::string &story_name, const int &flags);
    bool ReleaseStory(std::string &chronicle_name, std::string &story_name, const int &flags);
    std::string GetChronicleAttr(std::string &chronicle_name, const std::string &key);
    bool EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value);

private:
    std::shared_ptr<ChronicleMetadataRPCProxy> metadataRpcProxy_;
    std::shared_ptr<ChronoLogAdminRPCProxy> adminRpcProxy_;
};
#endif //CHRONOLOG_CLIENT_H
