//
// Created by kfeng on 7/11/22.
//

#include <unistd.h>
#include <mutex>
#include "ClientRegistryManager.h"
#include "errcode.h"
#include "log.h"
#include <iostream>

ClientRegistryManager::ClientRegistryManager() {
    LOGD("%s constructor is called, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());

    clientRegistry_ = new std::unordered_map<std::string, ClientInfo>();

    LOGD("clientRegistry_@%p has %ld entries", clientRegistry_, clientRegistry_->size());
}

ClientRegistryManager::~ClientRegistryManager() {
    delete clientRegistry_;
}

int ClientRegistryManager::add_client_record(const std::string &client_id, const ClientInfo &record) {
    LOGD("%s in ClientRegistryManager@%p", __FUNCTION__, this);
    LOGD("clientRegistry_@%p has %ld entries stored", clientRegistry_, clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    std::cout <<" add_client_record role = "<<record.client_role_<<std::endl;
    if (clientRegistry_->insert_or_assign(client_id, record).second) {
        LOGD("a new entry has been added to clientRegistry_@%p", clientRegistry_);
        return CL_SUCCESS;
    } else
        return CL_ERR_UNKNOWN;
}

int ClientRegistryManager::remove_client_record(const std::string &client_id, int &flags) {
    LOGD("%s in ClientRegistryManager@%p", __FUNCTION__, this);
    LOGD("clientRegistry_@%p has %ld entries", clientRegistry_, clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    if (clientRegistry_->erase(client_id)) {
        LOGD("an entry has been removed from clientRegistry_@%p", clientRegistry_);
        return CL_SUCCESS;
    } else
        return CL_ERR_UNKNOWN;
}

int ClientRegistryManager::update_client_role(std::string &client_id,uint32_t &role)
{
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    auto iter = clientRegistry_->find(client_id);
    int ret = CL_ERR_NOT_EXIST;
    if(iter != clientRegistry_->end())
    {
       (*iter).second.client_role_ = role;
	ret = CL_SUCCESS;
    }
    return ret;
}

int ClientRegistryManager::get_client_group_and_role(const std::string &client_id,std::string &group_id,uint32_t &role)
{
      std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
      auto it = clientRegistry_->find(client_id);
      int ret = CL_ERR_NOT_EXIST;
      if(it != clientRegistry_->end())
      {
	group_id = (*it).second.group_id_;
	role = (*it).second.client_role_;
	ret = CL_SUCCESS;
      }     
      return ret; 
}
