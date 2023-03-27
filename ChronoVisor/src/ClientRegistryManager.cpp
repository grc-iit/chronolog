//
// Created by kfeng on 7/11/22.
//

#include <unistd.h>
#include <mutex>
#include "ClientRegistryManager.h"
#include "errcode.h"
#include "log.h"
#include <ChronicleMetaDirectory.h>

ClientRegistryManager::ClientRegistryManager() {
    LOGD("%s constructor is called, object created@%p in thread PID=%d",
         typeid(*this).name(), this, getpid());

    clientRegistry_ = new std::unordered_map<std::string, ClientInfo>();

    LOGD("clientRegistry_@%p has %ld entries", clientRegistry_, clientRegistry_->size());
}

ClientRegistryManager::~ClientRegistryManager() {
    delete clientRegistry_;
}

ClientInfo& ClientRegistryManager::get_client_info(const std::string &client_id) {
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if (clientRegistryRecord != clientRegistry_->end()) {
        return clientRegistryRecord->second;
    }
}

int ClientRegistryManager::add_story_acquisition(const std::string &client_id, uint64_t &sid, Story *pStory) {
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if (clientRegistryRecord != clientRegistry_->end()) {
        auto clientInfo = clientRegistryRecord->second;
        if (clientInfo.acquiredStoryList_.find(sid) != clientInfo.acquiredStoryList_.end()) {
            return CL_ERR_ACQUIRED;
        } else {
            clientInfo.acquiredStoryList_.emplace(sid, pStory);
            auto res = clientRegistry_->insert_or_assign(client_id, clientInfo);
            if (res.second) {
                return CL_SUCCESS;
            } else {
                return CL_ERR_UNKNOWN;
            }
        }
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ClientRegistryManager::remove_story_acquisition(const std::string &client_id, uint64_t &sid) {
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if (clientRegistryRecord != clientRegistry_->end()) {
        auto clientInfo = clientRegistryRecord->second;
        if (clientInfo.acquiredStoryList_.find(sid) != clientInfo.acquiredStoryList_.end()) {
            return CL_ERR_ACQUIRED;
        } else {
            clientInfo.acquiredStoryList_.erase(sid);
        }
    } else {
        return CL_ERR_UNKNOWN;
    }
}

int ClientRegistryManager::add_client_record(const std::string &client_id, const ClientInfo &record) {
    LOGD("%s in ClientRegistryManager@%p", __FUNCTION__, this);
    LOGD("clientRegistry_@%p has %ld entries stored", clientRegistry_, clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
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
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if (clientRegistryRecord != clientRegistry_->end()) {
        auto clientInfo = clientRegistryRecord->second;
        if (!clientInfo.acquiredStoryList_.empty()) {
            return CL_ERR_ACQUIRED;
        }
    } else {
        return CL_ERR_UNKNOWN;
    }
    if (clientRegistry_->erase(client_id)) {
        LOGD("an entry has been removed from clientRegistry_@%p", clientRegistry_);
        return CL_SUCCESS;
    } else
        return CL_ERR_UNKNOWN;
}
