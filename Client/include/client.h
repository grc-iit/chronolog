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
		std::string &group_id,
		uint32_t &role,
                int &flags,
                uint64_t &clock_offset);
    int Disconnect(const std::string &client_id, int &flags);
    int CreateChronicle(std::string &name,
                        const std::unordered_map<std::string, std::string> &attrs,
                        int &flags);
    int DestroyChronicle(std::string &name, int &flags);
    int AcquireChronicle(std::string &name, int &flags);
    int ReleaseChronicle(std::string &name, int &flags);
    int UpdateChroniclePermissions(std::string &name, std::string &perm);
    int CreateStory(std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags);
    int DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int AcquireStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int UpdateStoryPermissions(std::string &chronicle_name,std::string &story_name,std::string &perm);
    int GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value);
    int SetClientId(std::string &client_id);
    std::string& GetClientId();
    int SetClientRole(uint32_t &role);
    int RequestRoleChange(uint32_t &role);
    int AddGrouptoChronicle(std::string &chronicle_name,std::string &new_group_id,std::string &new_perm);
    int RemoveGroupFromChronicle(std::string &chronicle_name,std::string &new_group_id);
    int AddGrouptoStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id,std::string &new_perm);
    int RemoveGroupFromStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id);
    int CheckClientRole(uint32_t &role);
    uint32_t GetClientRole();
    int SetGroupId(std::string &group_id);
    std::string &GetGroupId();

private:
    std::string clientid;
    std::shared_ptr<RPCClient> rpcClient_;
    ClocksourceManager *pClocksourceManager_;
    uint32_t my_role_;    
    std::string group_id_;
    std::shared_ptr<RPCClient> rpcProxy_;
};
#endif //CHRONOLOG_CLIENT_H
