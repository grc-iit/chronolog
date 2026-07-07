#ifndef INGESTION_QUEUE_H
#define INGESTION_QUEUE_H

#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
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
    //
    // The shared lock is held across handle->ingestEvent so the retirement
    // path (KeeperDataStore::retireDecayedPipelines: removeIngestionHandle
    // followed by `delete pipeline`) cannot tear down the handle mid-call.
    // Concurrent ingest threads still proceed in parallel since shared locks
    // don't block each other; only the rare retire path waits.
    void ingestLogEvent(LogEvent const& event)
    {
        std::shared_lock<std::shared_mutex> lock(handleMapMutex);
        LOG_TRACE("[IngestionQueue] Received event for StoryID={}: storyId={}, time={}, clientId={}, index={}, "
                  "record={}, HandleMapSize={}",
                  event.storyId,
                  event.getStoryId(),
                  event.time(),
                  event.getClientId(),
                  event.index(),
                  event.getRecord(),
                  storyIngestionHandles.size());
        auto ingestionHandle_iter = storyIngestionHandles.find(event.storyId);
        if(ingestionHandle_iter != storyIngestionHandles.end())
        {
            ingestionHandle_iter->second->ingestEvent(event);
            return;
        }
        lock.unlock();

        LOG_WARNING("[IngestionQueue] Orphan event for story {}. Storing for later processing.", event.storyId);
        std::lock_guard<std::mutex> orphan_lock(orphanQueueMutex);
        orphanEventQueue.push_back(event);
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

    // Distinct story ids that currently have orphaned (un-routable) events.
    // The DataStore uses this to find late events for retired stories that can
    // no longer be re-homed into a live ingestion handle, so it can seal them
    // for archival instead of leaving them to be dropped.
    std::unordered_set<StoryId> orphanStoryIds() const
    {
        std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
        std::unordered_set<StoryId> ids;
        for(auto const& orphan_event: orphanEventQueue)
        { ids.insert(orphan_event.storyId); }
        return ids;
    }

    // Remove and return all orphaned events for a single story (used by the
    // DataStore right before it seals them into a recovery StoryChunk).
    std::deque<LogEvent> extractOrphansForStory(StoryId const& story_id)
    {
        std::deque<LogEvent> extracted;
        std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
        for(auto iter = orphanEventQueue.begin(); iter != orphanEventQueue.end();)
        {
            if((*iter).storyId == story_id)
            {
                extracted.push_back(*iter);
                iter = orphanEventQueue.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
        return extracted;
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

        // Any events still orphaned here belong to stories that have no
        // ingestion handle (retired and never re-acquired, or never acquired),
        // so they can no longer be sealed and would otherwise be dropped
        // silently when this queue is destroyed. We cannot re-home or archive
        // them (an orphan LogEvent carries only its storyId, not the
        // chronicle/story names needed to build an archivable StoryChunk), so
        // at minimum surface the loss with an error rather than dropping it
        // silently.
        {
            std::lock_guard<std::mutex> orphanLock(orphanQueueMutex);
            if(!orphanEventQueue.empty())
            {
                std::unordered_map<StoryId, std::size_t> per_story;
                for(auto const& orphan_event: orphanEventQueue)
                { per_story[orphan_event.storyId]++; }
                std::stringstream story_breakdown;
                for(auto const& entry: per_story)
                { story_breakdown << " story=" << entry.first << ":" << entry.second; }
                LOG_ERROR("[IngestionQueue] Shutdown is dropping {} un-drainable orphan event(s) across {} "
                          "story(ies) that had no ingestion handle to seal them:{}",
                          orphanEventQueue.size(),
                          per_story.size(),
                          story_breakdown.str());
            }
        }

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
