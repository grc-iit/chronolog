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
        uint32_t mask = 7;
        uint32_t user_role = role & mask;
        if(user_role == CHRONOLOG_CLIENT_RWCD) return true;
        else return false;
    }
    inline bool can_write(uint32_t& role)
    {
           uint32_t mask = 7;
           uint32_t user_role = role & mask;
           if(user_role == CHRONOLOG_CLIENT_RW || user_role == CHRONOLOG_CLIENT_RWCD) return true;
           else return false;
    }
    inline bool can_read(uint32_t& role)
    {
        uint32_t mask = 7;
        uint32_t user_role = role & mask;
        if(user_role == CHRONOLOG_CLIENT_RO || CHRONOLOG_CLIENT_RW || user_role == CHRONOLOG_CLIENT_RWCD) return true;
        else return false;
    }
    inline bool can_perform_fileops(uint32_t &role)
   {
        uint32_t mask = 7;
        uint32_t user_role = role & mask;
        mask = mask << 3;
        uint32_t group_role = role & mask;
        group_role = group_role >> 3;
        if(user_role == CHRONOLOG_CLIENT_RWCD && group_role == CHRONOLOG_CLIENT_GROUP_ADMIN) return true;
        return false;
   }
    inline bool can_edit_ownership(uint32_t &role)
    {
	uint32_t mask = 7;
	uint32_t user_role = role & mask;
	mask = mask << 3;
	uint32_t group_role = role & mask;
	group_role = group_role >> 3;
	if(group_role == CHRONOLOG_CLIENT_GROUP_ADMIN) return true;
	return false;
    }

private:
    std::unordered_map<std::string, ClientInfo> *clientRegistry_;
    std::mutex g_clientRegistryMutex_;
};

#endif //CHRONOLOG_CLIENTREGISTRYMANAGER_H
