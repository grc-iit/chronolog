//
// Created by kfeng on 5/17/22.
//

#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include "RPCClient.h"
#include "errcode.h"
#include "ConfigurationManager.h"
#include "ClocksourceManager.h"

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;

class ChronoLogClient {
public:
    explicit ChronoLogClient(const std::string& conf_file_path = "") {
        if (!conf_file_path.empty())
            CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
        init();
    }

    explicit ChronoLogClient(const ChronoLog::ConfigurationManager& conf_manager) {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        init();
    }

    ChronoLogClient(const ChronoLogRPCImplementation& protocol, const std::string& visor_ip, int visor_port) {
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION = protocol;
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP = visor_ip;
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = visor_port;
        init();
    }

    void init() {
        pClocksourceManager_ = ClocksourceManager::getInstance();
        pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);
        CHRONOLOG_CONF->ROLE = CHRONOLOG_CLIENT;
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
    ClocksourceManager *pClocksourceManager_;
};
#endif //CHRONOLOG_CLIENT_H
