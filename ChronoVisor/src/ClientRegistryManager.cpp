//
// Created by kfeng on 7/11/22.
//
#include "ClientRegistryManager.h"
#include "singleton.h"

ClientRegistryManager::ClientRegistryManager() {
    clientRegistry_ = ChronoLog::Singleton<std::unordered_map<std::string, ClientRegistryRecord>>::GetInstance();
}

ClientRegistryManager::~ClientRegistryManager() = default;

bool ClientRegistryManager::connect(const std::string& client_id, const ClientRegistryRecord &record) {
    clientRegistry_->emplace(std::make_pair(client_id, record));
    return true;
}

bool ClientRegistryManager::disconnect(const std::string& client_id) {
    if (clientRegistry_)
        return clientRegistry_->erase(client_id);
    return false;
}