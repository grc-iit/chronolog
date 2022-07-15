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
        metadataRpcProxy_ = ChronicleMetadataRPCProxy();
        adminRpcProxy_ = ChronoLogAdminRPCProxy();
    }

    bool Connect(const std::string &server_uri, std::string &client_id);
    bool Disconnect(const std::string &client_id, int &flags);
    bool CreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs);
    bool DestroyChronicle(std::string &name, const int &flags);
    bool AcquireChronicle(std::string &name, const int &flags);
    bool ReleaseChronicle(uint64_t &cid, const int &flags);
    bool CreateStory(uint64_t &cid, std::string &name, const std::unordered_map<std::string, std::string> &attrs);
    bool DestroyStory(uint64_t &cid, std::string &name, const int &flags);
    bool AcquireStory(uint64_t &cid, std::string &name, const int &flags);
    bool ReleaseStory(uint64_t &sid, const int &flags);
    std::string GetChronicleAttr(uint64_t &cid, const std::string &key);
    bool EditChronicleAttr(uint64_t &cid, const std::string &key, const std::string &value);

private:
    ChronicleMetadataRPCProxy metadataRpcProxy_;
    ChronoLogAdminRPCProxy adminRpcProxy_;
};
#endif //CHRONOLOG_CLIENT_H
