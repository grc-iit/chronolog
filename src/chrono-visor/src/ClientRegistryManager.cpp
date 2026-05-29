//
// Created by kfeng on 7/11/22.
//

#include <unistd.h>
#include <mutex>
#include <sstream>

#include <ClientRegistryManager.h>
#include <chronolog_errcode.h>
#include <chrono_monitor.h>
#include <ChronicleMetaDirectory.h>

namespace chl = chronolog;

//////////////////////

ClientRegistryManager::ClientRegistryManager()
{
    std::stringstream ss;
    ss << this;
    LOG_DEBUG("[ClientRegistryManager] Constructor is called. Object created at {} in thread PID={}",
              ss.str(),
              getpid());
    clientRegistry_ = new std::unordered_map<chronolog::ClientId, ClientInfo>();
    std::stringstream s1;
    s1 << clientRegistry_;
    LOG_DEBUG("[ClientRegistryManager] Client Registry created at {} has {} entries",
              s1.str(),
              clientRegistry_->size());
}

ClientRegistryManager::~ClientRegistryManager() { delete clientRegistry_; }

ClientInfo* ClientRegistryManager::get_client_info(chl::ClientId const& client_id)
{
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        return &(clientRegistryRecord->second);
    }
    else
    {
        return nullptr;
    }
}
//////////////////////

int ClientRegistryManager::get_acquired_stories_snapshot(chl::ClientId const& client_id,
                                                         std::vector<std::pair<uint64_t, Story*>>& snapshot)
{
    snapshot.clear();
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord == clientRegistry_->end())
    {
        return chronolog::CL_ERR_NOT_EXIST;
    }
    auto const& acquiredStoryList = clientRegistryRecord->second.acquiredStoryList_;
    snapshot.reserve(acquiredStoryList.size());
    snapshot.assign(acquiredStoryList.begin(), acquiredStoryList.end());
    return chronolog::CL_SUCCESS;
}
//////////////////////

int ClientRegistryManager::add_story_acquisition(chl::ClientId const& client_id, uint64_t& sid, Story* pStory)
{
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        auto clientInfo = clientRegistryRecord->second;
        if(clientInfo.acquiredStoryList_.find(sid) != clientInfo.acquiredStoryList_.end())
        {
            LOG_DEBUG("[ClientRegistryManager] ClientID={} has already acquired StoryID={}", client_id, sid);
            return chronolog::CL_ERR_ACQUIRED;
        }
        else
        {
            clientInfo.acquiredStoryList_.emplace(sid, pStory);
            auto res = clientRegistry_->insert_or_assign(client_id, clientInfo);
            if(res.second)
            {
                LOG_DEBUG("[ClientRegistryManager] Added a new entry for ClientID={} acquiring StoryID={}",
                          client_id,
                          sid);
                return chronolog::CL_SUCCESS;
            }
            else
            {
                LOG_DEBUG("[ClientRegistryManager] Updated an existing entry for ClientID={} acquiring StoryID={}",
                          client_id,
                          sid);
                return chronolog::CL_ERR_UNKNOWN;
            }
        }
    }
    else
    {
        LOG_DEBUG("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}

//////////////////////

int ClientRegistryManager::remove_story_acquisition(chl::ClientId const& client_id, uint64_t& sid)
{
    std::lock_guard<std::mutex> clientRegistryLock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        if(clientRegistryRecord->second.acquiredStoryList_.find(sid) !=
           clientRegistryRecord->second.acquiredStoryList_.end())
        {
            auto nErased = clientRegistryRecord->second.acquiredStoryList_.erase(sid);
            if(nErased == 1)
            {
                LOG_DEBUG("[ClientRegistryManager] Removed StoryID={} from AcquiredStoryList for ClientID={}",
                          sid,
                          client_id);
                LOG_DEBUG("[ClientRegistryManager] Acquired Story List of ClientID={} has {} entries left",
                          client_id,
                          clientRegistryRecord->second.acquiredStoryList_.size());
                return chronolog::CL_SUCCESS;
            }
            else
            {
                LOG_DEBUG("[ClientRegistryManager] Failed to remove StoryID={} from AcquiredStoryList for ClientID={}",
                          sid,
                          client_id);
                return chronolog::CL_ERR_UNKNOWN;
            }
        }
        else
        {
            LOG_WARNING("[ClientRegistryManager] StoryID={} is not acquired by ClientID={}, cannot remove from "
                        "AcquiredStoryList for it",
                        sid,
                        client_id);
            return chronolog::CL_ERR_NOT_ACQUIRED;
        }
    }
    else
    {
        LOG_ERROR("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int ClientRegistryManager::add_client_record(chl::ClientId const& client_id, const ClientInfo& record)
{
    LOG_DEBUG("[ClientRegistryManager] ClientRecord added in ClientRegistryManager at {}",
              static_cast<const void*>(this));
    LOG_DEBUG("[ClientRegistryManager] Client Registry at {} has {} entries stored",
              static_cast<void*>(clientRegistry_),
              clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    // Refuse to overwrite an existing entry. Two clients ending up with the same
    // packed ClientId (ip<<32 | port<<16 | pid&0xFFFF) is possible — the pid is
    // truncated to 16 bits and unresolvable hostnames pack ip=0 — so we surface
    // the collision instead of silently aliasing two clients onto one record.
    auto [it, inserted] = clientRegistry_->insert({client_id, record});
    if(!inserted)
    {
        LOG_WARNING("[ClientRegistryManager] Refusing duplicate ClientID={}: a client with the same packed "
                    "(ip, port, instance) is already registered. The new client should re-Connect once the "
                    "incumbent disconnects, or rebuild its identity with non-colliding fields.",
                    client_id);
        return chronolog::CL_ERR_INVALID_ARG;
    }
    LOG_DEBUG("[ClientRegistryManager] An entry for ClientID={} has been added to Client Registry at {}",
              client_id,
              static_cast<void*>(clientRegistry_));
    LOG_DEBUG("[ClientRegistryManager] Client Registry at {} has {} entries stored",
              static_cast<void*>(clientRegistry_),
              clientRegistry_->size());
    return chronolog::CL_SUCCESS;
}

int ClientRegistryManager::remove_client_record(const chronolog::ClientId& client_id)
{
    LOG_DEBUG("[ClientRegistryManager] Remove client record in ClientRegistryManager at {}",
              static_cast<const void*>(this));
    LOG_DEBUG("[ClientRegistryManager] Client Registry at {} has {} entries",
              static_cast<void*>(clientRegistry_),
              clientRegistry_->size());
    std::lock_guard<std::mutex> lock(g_clientRegistryMutex_);
    auto clientRegistryRecord = clientRegistry_->find(client_id);
    if(clientRegistryRecord != clientRegistry_->end())
    {
        auto clientInfo = clientRegistryRecord->second;
        if(!clientInfo.acquiredStoryList_.empty())
        {
            LOG_WARNING("[ClientRegistryManager] ClientID={} still has {} stories acquired, entry cannot be removed",
                        client_id,
                        clientInfo.acquiredStoryList_.size());
            return chronolog::CL_ERR_ACQUIRED;
        }
    }
    else
    {
        LOG_ERROR("[ClientRegistryManager] ClientID={} does not exist", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
    if(clientRegistry_->erase(client_id))
    {
        LOG_DEBUG("[ClientRegistryManager] Entry for ClientID={} has been removed from clientRegistry_ at {}",
                  client_id,
                  static_cast<void*>(clientRegistry_));
        return chronolog::CL_SUCCESS;
    }
    else
    {
        LOG_ERROR("[ClientRegistryManager] Failed to remove ClientID={} from clientRegistry_", client_id);
        return chronolog::CL_ERR_UNKNOWN;
    }
}
