#ifndef INGESTION_QUEUE_H
#define INGESTION_QUEUE_H

#include <iostream>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sstream>

#include <chrono_monitor.h>
#include <chronolog_types.h>

#include "StoryIngestionHandle.h"

//
// IngestionQueue is a funnel into the MemoryDataStore
// std::deque guarantees O(1) time for addidng elements and resizing
// (vector of vectors implementation)

namespace chronolog
{

typedef std::deque<LogEvent> EventDeque;

class IngestionQueue
{
public:
    IngestionQueue() {}

    ~IngestionQueue() { shutDown(); }

    void addStoryIngestionHandle(StoryId const& story_id, StoryIngestionHandle* ingestion_handle)
    {
        std::unique_lock<std::shared_mutex> lock(handleMapMutex);
        storyIngestionHandles.emplace(story_id, ingestion_handle);
        LOG_DEBUG("[IngestionQueue] Added handle for StoryID={}: HandleAddress={}, StoryIngestionHandles={}, "
                  "HandleMapSize={}",
                  story_id,
                  static_cast<void*>(ingestion_handle),
                  reinterpret_cast<void*>(&storyIngestionHandles),
                  storyIngestionHandles.size());
    }

    void removeIngestionHandle(StoryId const& story_id)
    {
        std::unique_lock<std::shared_mutex> lock(handleMapMutex);
        if(storyIngestionHandles.erase(story_id))
        {
            LOG_DEBUG("[IngestionQueue] Removed handle for StoryID={}. Current handle MapSize={}",
                      story_id,
                      storyIngestionHandles.size());
        }
        else
        {
            LOG_WARNING("[IngestionQueue] Tried to remove non-existent handle for StoryID={}.", story_id);
        }
    }

    // Hot path: called concurrently by the keeper's RPC handler threads
    // (rpc_thread_count = INGESTION_THREAD_COUNT). The map lookup is guarded
    // by a shared lock so multiple ingest threads can resolve handles in
    // parallel; only add/removeIngestionHandle take an exclusive lock.
    // The handle itself is invoked outside the map lock so handler threads
    // don't serialize on it -- StoryIngestionHandle::ingestEvent has its own
    // per-story mutex.
    void ingestLogEvent(LogEvent const& event)
    {
        StoryIngestionHandle* handle = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(handleMapMutex);
#ifndef NDEBUG
            std::stringstream ss;
            ss << event;
            LOG_TRACE("[IngestionQueue] Received event for StoryID={}: Event Details={}, HandleMapSize={}",
                      event.storyId,
                      ss.str(),
                      storyIngestionHandles.size());
#endif
            auto ingestionHandle_iter = storyIngestionHandles.find(event.storyId);
            if(ingestionHandle_iter != storyIngestionHandles.end())
                handle = ingestionHandle_iter->second;
        }

        if(handle != nullptr)
        {
            handle->ingestEvent(event);
        }
        else
        {
            LOG_WARNING("[IngestionQueue] Orphan event for story {}. Storing for later processing.", event.storyId);
            std::lock_guard<std::mutex> lock(orphanQueueMutex);
            orphanEventQueue.push_back(event);
        }
    }

    // Lock ordering when both mutexes are needed: orphanQueueMutex first,
    // then handleMapMutex. ingestLogEvent never holds them simultaneously, so
    // there is no inverse-order acquisition path.
    void drainOrphanEvents()
    {
        std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
        if(orphanEventQueue.empty())
        {
            LOG_DEBUG("[IngestionQueue] Orphan event queue is empty. No actions taken.");
            return;
        }
        std::shared_lock<std::shared_mutex> mapLock(handleMapMutex);
        for(EventDeque::iterator iter = orphanEventQueue.begin(); iter != orphanEventQueue.end();)
        {
            auto ingestionHandle_iter = storyIngestionHandles.find((*iter).storyId);
            if(ingestionHandle_iter != storyIngestionHandles.end())
            {
                // Individual StoryIngestionHandle has its own mutex
                (*ingestionHandle_iter).second->ingestEvent(*iter);
                // Remove the event from the orphan deque and get the iterator to the next element prior to removal
                iter = orphanEventQueue.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
        LOG_DEBUG("[IngestionQueue] Drained {} orphan events into known handles.", orphanEventQueue.size());
    }

    bool is_empty() const
    {
        std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
        std::shared_lock<std::shared_mutex> mapLock(handleMapMutex);
        return (orphanEventQueue.empty() && storyIngestionHandles.empty());
    }

    void shutDown()
    {
        {
            std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
            std::shared_lock<std::shared_mutex> mapLock(handleMapMutex);
            LOG_INFO("[IngestionQueue] Initiating shutdown. HandleMapSize={}, Orphan EventQueueSize={}",
                     storyIngestionHandles.size(),
                     orphanEventQueue.size());
        }
        // last attempt to drain orphanEventQueue into known ingestionHandles
        drainOrphanEvents();
        // disengage all handles
        std::unique_lock<std::shared_mutex> lock(handleMapMutex);
        storyIngestionHandles.clear();
        LOG_INFO("[IngestionQueue] Shutdown completed. All handles disengaged.");
    }

private:
    IngestionQueue(IngestionQueue const&) = delete;

    IngestionQueue& operator=(IngestionQueue const&) = delete;

    // handleMapMutex is shared/exclusive: ingestLogEvent and drainOrphanEvents
    // take shared locks (concurrent map lookups); add/remove take exclusive.
    // mutable so const observers (is_empty) can take a shared lock.
    mutable std::shared_mutex handleMapMutex;
    std::unordered_map<StoryId, StoryIngestionHandle*> storyIngestionHandles;

    // events for unknown stories or late events for closed stories will end up
    // in orphanEventQueue that we'll periodically try to drain into the DataStore
    mutable std::mutex orphanQueueMutex;
    std::deque<LogEvent> orphanEventQueue;

    //Timer to triger periodic attempt to drain orphanEventQueue and collect/log statistics
};
} // namespace chronolog

#endif
