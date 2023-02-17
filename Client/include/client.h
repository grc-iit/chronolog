//
// Created by kfeng on 5/17/22.
//

#ifndef CHRONOLOG_CLIENT_H
#define CHRONOLOG_CLIENT_H

#include "RPCClient.h"
#include "errcode.h"
#include "ClocksourceManager.h"

ClocksourceManager *ClocksourceManager::clocksourceManager_ = nullptr;

class ChronoLogClient {
public:
    ChronoLogClient() { CHRONOLOG_CONF->ConfigureDefaultClient("../../test/communication/server_list"); }

    ChronoLogClient(const std::string& server_list_file_path) {
        CHRONOLOG_CONF->ConfigureDefaultClient(server_list_file_path);
        rpcProxy_ = ChronoLog::Singleton<RPCClient>::GetInstance();
    }

    ChronoLogClient(const ChronoLogRPCImplementation& protocol, const std::string& visor_ip, int visor_port) {
        CHRONOLOG_CONF->IS_SERVER = false;
        CHRONOLOG_CONF->RPC_IMPLEMENTATION = protocol;
        CHRONOLOG_CONF->RPC_SERVER_IP = visor_ip;
        CHRONOLOG_CONF->RPC_BASE_SERVER_PORT = visor_port;
        rpcProxy_ = ChronoLog::Singleton<RPCClient>::GetInstance();
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
    int CreateStory(std::string &chronicle_name,
                    std::string &story_name,
                    const std::unordered_map<std::string, std::string> &attrs,
                    int &flags);
    int DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int AcquireStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags);
    int GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value);
    int EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value);
    int SetClientId(std::string &client_id);
    std::string& GetClientId();
    int SetClientRole(uint32_t &role);
    int CreateClientRole(uint32_t &user, uint32_t &group,uint32_t &cluster);
    int RequestRoleChange(uint32_t &role);
    int AddGrouptoChronicle(std::string &chronicle_name,std::string &new_group_id);
    int RemoveGroupFromChronicle(std::string &chronicle_name,std::string &new_group_id);
    int AddGrouptoStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id);
    int RemoveGroupFromStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id);
    int CheckClientRole(uint32_t &role);
    uint32_t GetClientRole();
    uint32_t UserRole();
    uint32_t GroupRole();
    uint32_t ClusterRole();
    int SetGroupId(std::string &group_id);
    std::string &GetGroupId();

private:
    std::string client_id_;
    uint32_t my_role_;    
    std::string group_id_;
    std::shared_ptr<RPCClient> rpcProxy_;
};
#endif //CHRONOLOG_CLIENT_H
