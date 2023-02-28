//
// Created by kfeng on 7/11/22.
//

#ifndef CHRONOLOG_CLIENTREGISTRYMANAGER_H
#define CHRONOLOG_CLIENTREGISTRYMANAGER_H

#include <unordered_map>
#include <vector>
#include <memory>
#include <ClientInfo.h>
#include <enum.h>
#include "city.h"

class ClientRegistryManager {
public:
    ClientRegistryManager();
    ~ClientRegistryManager();

    int add_client_record(const std::string &client_id, const ClientInfo &record);
    int remove_client_record(const std::string& client_id, int &flags);
    int get_client_group_and_role(const std::string &client_id, std::string &group_id,uint32_t &role);
    int update_client_role(std::string &client_id,uint32_t &role);

    inline bool can_create_or_delete(uint32_t& role)
    {
        if(role == CHRONOLOG_CLIENT_REGULAR_USER || role == CHRONOLOG_CLIENT_PRIVILEGED_USER) return true;
        return false;
    }
    inline bool can_write(uint32_t& role)
    {
           if(role == CHRONOLOG_CLIENT_REGULAR_USER || role == CHRONOLOG_CLIENT_PRIVILEGED_USER) return true;
           return false;
    }
    inline bool can_read(uint32_t& role)
    {
	if(role == CHRONOLOG_CLIENT_RESTRICTED_USER || role == CHRONOLOG_CLIENT_REGULAR_USER || role == CHRONOLOG_CLIENT_PRIVILEGED_USER) return true;
        return false;
    }
    inline bool can_perform_fileops(uint32_t &role)
   {
	if(role == CHRONOLOG_CLIENT_PRIVILEGED_USER || role == CHRONOLOG_CLIENT_CLUSTER_ADMIN) return true;
        return false;
   }
    inline bool can_edit_ownership(uint32_t &role)
    {
	if(role == CHRONOLOG_CLIENT_PRIVILEGED_USER || role == CHRONOLOG_CLIENT_CLUSTER_ADMIN) return true;
	return false;
    }
    
private:
    std::unordered_map<std::string, ClientInfo,stringhashfn> *clientRegistry_;
    std::mutex g_clientRegistryMutex_;
};

#endif //CHRONOLOG_CLIENTREGISTRYMANAGER_H
