//
// Created by kfeng on 7/11/22.
//

#ifndef CHRONOLOG_CLIENTREGISTRYMANAGER_H
#define CHRONOLOG_CLIENTREGISTRYMANAGER_H

#include <unordered_map>
#include <vector>
#include <memory>
#include <ClientInfo.h>

class ClientRegistryManager {
public:
    ClientRegistryManager();
    ~ClientRegistryManager();

    int add_client_record(const std::string &client_id, const ClientInfo &record);
    int remove_client_record(const std::string& client_id, int &flags);
    int get_client_group_and_role(const std::string &client_id, std::string &group_id,uint32_t &role);

private:
    std::unordered_map<std::string, ClientInfo> *clientRegistry_;
    std::mutex g_clientRegistryMutex_;
};

#endif //CHRONOLOG_CLIENTREGISTRYMANAGER_H
