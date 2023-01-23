//
// Created by kfeng on 5/17/22.
//

#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include "RPCClient.h"
#include "errcode.h"
#include "ClocksourceManager.h"
#include "ConfigurationManager.h"

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;

class ChronoLogClient {
public:
    explicit ChronoLogClient(const std::string& conf_file_path = "") {
        if (!conf_file_path.empty())
            CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
        rpcClient_ = ChronoLog::Singleton<RPCClient>::GetInstance();
    }

    explicit ChronoLogClient(const ChronoLog::ConfigurationManager& conf_manager) {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        rpcClient_ = ChronoLog::Singleton<RPCClient>::GetInstance();
    }

    ChronoLogClient(const ChronoLogRPCImplementation& protocol, const std::string& visor_ip, int visor_port) {
        CHRONOLOG_CONF->IS_VISOR = false;
        CHRONOLOG_CONF->RPC_IMPLEMENTATION = protocol;
        CHRONOLOG_CONF->RPC_VISOR_IP = visor_ip;
        CHRONOLOG_CONF->RPC_BASE_VISOR_PORT = visor_port;
        rpcClient_ = ChronoLog::Singleton<RPCClient>::GetInstance();
    }

    int Connect(const std::string &server_uri,
                std::string &client_id,
                int &flags,
                uint64_t &clock_offset);
    int Disconnect(const std::string &client_id, int &flags);
    int CreateChronicle(std::string &name,
                        const std::unordered_map<std::string, std::string> &attrs,
                        int &flags);
    int DestroyChronicle(std::string &name, int &flags);
    int AcquireChronicle(std::string &name, int &flags);
    int ReleaseChronicle(std::string &name, int &flags);
    int CreateStory(std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags);
    int DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int AcquireStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value);

private:
    std::shared_ptr<RPCClient> rpcClient_;
};
#endif //CHRONOLOG_CLIENT_H
