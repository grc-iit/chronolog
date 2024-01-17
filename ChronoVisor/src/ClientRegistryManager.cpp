//
// Created by kfeng on 7/11/22.
//

#include <unistd.h>
#include <mutex>
#include "ClientRegistryManager.h"
#include "chronolog_errcode.h"
#include "log.h"
#include <ChronicleMetaDirectory.h>

namespace chl = chronolog;

//////////////////////

ClientRegistryManager::ClientRegistryManager()
{
    std::stringstream ss;
    ss << this;
    LOGD("[ClientRegistryManager] Constructor is called. Object created at {} in thread PID={}", ss.str(), getpid());
    clientRegistry_ = new std::unordered_map <chronolog::ClientId, ClientInfo>();
    std::stringstream s1;
    s1 << clientRegistry_;
    LOGD("[ClientRegistryManager] Client Registry created at {} has {} entries", s1.str(), clientRegistry_->size());
}

ClientRegistryManager::~ClientRegistryManager()
{
    delete clientRegistry_;
}

ClientInfo*ClientRegistryManager::get_client_info(chl::ClientId const &client_id)
{
    std::lock_guard <std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        return &(clientRegistryRecord->second);
    }
    else
    { return nullptr; }
}
//////////////////////

int ClientRegistryManager::add_story_acquisition(chl::ClientId const &client_id, uint64_t &sid, Story*pStory)
{
    std::lock_guard <std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        auto clientInfo = clientRegistryRecord->second;
        if(clientInfo.acquiredStoryList_.find(sid) != clientInfo.acquiredStoryList_.end())
        {
            LOGD("[ClientRegistryManager] ClientID={} has already acquired StoryID={}", client_id, sid);
            return chronolog::CL_ERR_ACQUIRED;
        }
        else
        {
            clientInfo.acquiredStoryList_.emplace(sid, pStory);
            auto res = clientRegistry_->insert_or_assign(client_id, clientInfo);
            if(res.second)
            {
                LOGD("[ClientRegistryManager] Added a new entry for ClientID={} acquiring StoryID={}", client_id, sid);
                return chronolog::CL_SUCCESS;
            }
            else
            {
                LOGD("[ClientRegistryManager] Updated an existing entry for ClientID={} acquiring StoryID={}", client_id
                     , sid);
                return chronolog::CL_ERR_UNKNOWN;
            }
        }
    }
    else
    {
        LOGD("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}

//////////////////////

int ClientRegistryManager::remove_story_acquisition(chl::ClientId const &client_id, uint64_t &sid)
{
    std::lock_guard <std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        if(clientRegistryRecord->second.acquiredStoryList_.find(sid) !=
           clientRegistryRecord->second.acquiredStoryList_.end())
        {
            auto nErased = clientRegistryRecord->second.acquiredStoryList_.erase(sid);
            if(nErased == 1)
            {
                LOGD("[ClientRegistryManager] Removed StoryID={} from AcquiredStoryList for ClientID={}", sid
                     , client_id);
                LOGD("[ClientRegistryManager] Acquired Story List of ClientID={} has {} entries left", client_id
                     , clientRegistryRecord->second.acquiredStoryList_.size());
                return chronolog::CL_SUCCESS;
            }
            else
            {
                LOGD("[ClientRegistryManager] Failed to remove StoryID={} from AcquiredStoryList for ClientID={}", sid
                     , client_id);
                return chronolog::CL_ERR_UNKNOWN;
            }
        }
        else
        {
            LOGW("[ClientRegistryManager] StoryID={} is not acquired by ClientID={}, cannot remove from AcquiredStoryList for it"
                 , sid, client_id);
            return chronolog::CL_ERR_NOT_ACQUIRED;
        }
    }
    else
    {
        LOGE("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int ClientRegistryManager::add_client_record(chl::ClientId const &client_id, const ClientInfo &record)
{
    LOGD("[ClientRegistryManager] ClientRecord added in ClientRegistryManager at {}", static_cast<const void*>(this));
    LOGD("[ClientRegistryManager] Client Registry at {} has {} entries stored", static_cast<void*>(clientRegistry_)
         , clientRegistry_->size());
    std::lock_guard <std::mutex> lock(g_clientRegistryMutex_);
    if(clientRegistry_->insert_or_assign(client_id, record).second)
    {
        LOGD("[ClientRegistryManager] An entry for ClientID={} has been added to Client Registry at {}", client_id
             , static_cast<void*>(clientRegistry_));
        LOGD("[ClientRegistryManager] Client Registry at {} has {} entries stored", static_cast<void*>(clientRegistry_)
             , clientRegistry_->size());
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOGE("[ClientRegistryManager] Fail to add entry for ClientID={} to clientRegistry_", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int ClientRegistryManager::remove_client_record(const chronolog::ClientId &client_id)
{
    LOGD("[ClientRegistryManager] Remove client record in ClientRegistryManager at {}", static_cast<const void*>(this));
    LOGD("[ClientRegistryManager] Client Registry at {} has {} entries", static_cast<void*>(clientRegistry_)
         , clientRegistry_->size());
    std::lock_guard <std::mutex> lock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        auto clientInfo = clientRegistryRecord->second;
        if(!clientInfo.acquiredStoryList_.empty())
        {
            LOGW("[ClientRegistryManager] ClientID={} still has {} stories acquired, entry cannot be removed", client_id
                 , clientInfo.acquiredStoryList_.size());
            return chronolog::CL_ERR_ACQUIRED;
        }
    }
    else
    {
        LOGE("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
    if(clientRegistry_->erase(client_id))
    {
        LOGD("[ClientRegistryManager] Entry for ClientID={} has been removed from clientRegistry_ at {}", client_id
             , static_cast<void*>(clientRegistry_));
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOGE("[ClientRegistryManager] Failed to remove ClientID={} from clientRegistry_", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
