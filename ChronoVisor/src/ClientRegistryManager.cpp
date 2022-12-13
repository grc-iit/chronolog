//
// Created by kfeng on 7/11/22.
//

#include <sys/types.h>
#include <unistd.h>
#include <mutex>
#include <cassert>
#include "ClientRegistryManager.h"
#include "singleton.h"
#include "log.h"

extern std::mutex g_clientRegistryMutex_;

ClientRegistryManager::ClientRegistryManager() {
    LOGD("%s constructor is called, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());

    clientRegistry_ = new std::unordered_map<std::string, ClientRegistryInfo>();

    LOGD("clientRegistry_@%p has %ld entries", clientRegistry_, clientRegistry_->size());
}

ClientRegistryManager::~ClientRegistryManager() {
    delete clientRegistry_;
}

bool ClientRegistryManager::add_client_record(const std::string &client_id, const ClientRegistryInfo &record) {
    LOGD("%s in ClientRegistryManager@%p", __FUNCTION__, this);
    LOGD("clientRegistry_@%p has %ld entries stored", clientRegistry_, clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    if (clientRegistry_->insert_or_assign(client_id, record).second) {
        LOGD("a new entry has been added to clientRegistry_@%p", clientRegistry_);
        return true;
    } else
        return false;
}

bool ClientRegistryManager::remove_client_record(const std::string &client_id, int &flags) {
    LOGD("%s in ClientRegistryManager@%p", __FUNCTION__, this);
    LOGD("clientRegistry_@%p has %ld entries", clientRegistry_, clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    if (clientRegistry_->erase(client_id)) {
        LOGD("an entry has been removed from clientRegistry_@%p", clientRegistry_);
        return true;
    } else
        return false;
}