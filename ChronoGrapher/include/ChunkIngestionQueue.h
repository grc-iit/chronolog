#ifndef CHUNK_INGESTION_QUEUE_H
#define CHUNK_INGESTION_QUEUE_H

#include <iostream>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <chrono>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <chronolog_profile.h>
#include <StoryChunk.h>
#include <StoryChunkIngestionHandle.h>

//
// IngestionQueue is a funnel into the MemoryDataStore
// std::deque guarantees O(1) time for addidng elements and resizing
// (vector of vectors implementation)

namespace chronolog
{

class ChunkIngestionQueue
{
public:
    ChunkIngestionQueue()
    {}

    ~ChunkIngestionQueue()
    { shutDown(); }

    void addStoryIngestionHandle(StoryId const &story_id, StoryChunkIngestionHandle*ingestion_handle)
    {
        CL_PROFILE_REGION("grapher_queue_register_story");
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyDrainCompletionCounts.erase(story_id);
        storyIngestionHandles.emplace(std::pair <StoryId, StoryChunkIngestionHandle*>(story_id, ingestion_handle));
        CL_PROFILE_COUNTER("grapher_ingestion_handle_count", storyIngestionHandles.size());
        LOG_DEBUG("[IngestionQueue] Added handle for StoryID={}: HandleAddress={}, StoryIngestionHandles={}, HandleMapSize={}"
             , story_id, static_cast<void*>(ingestion_handle), reinterpret_cast<void*>(&storyIngestionHandles)
             , storyIngestionHandles.size());
    }

    void removeStoryIngestionHandle(StoryId const &story_id)
    {
        CL_PROFILE_REGION("grapher_queue_unregister_story");
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        if(storyIngestionHandles.erase(story_id))
        {
            CL_PROFILE_COUNTER("grapher_ingestion_handle_count", storyIngestionHandles.size());
            LOG_DEBUG("[IngestionQueue] Removed handle for StoryID={}. Current handle MapSize={}", story_id
                 , storyIngestionHandles.size());
        }
        else
        {
            LOG_WARNING("[IngestionQueue] Tried to remove non-existent handle for StoryID={}.", story_id);
        }
    }

    void ingestStoryChunk(StoryChunk* chunk)
    {
        CL_PROFILE_REGION("grapher_queue_push");
        CL_PROFILE_COUNTER("grapher_queue_push_events", chunk->getEventCount());
        {
            std::lock_guard <std::mutex> lock(ingestionQueueMutex);
            LOG_DEBUG("[IngestionQueue] has {} StoryHandles; Received chunk for StoryID={} startTime {} eventCount{}",
                      storyIngestionHandles.size(),
                      chunk->getStoryId(),
                      chunk->getStartTime(),
                      chunk->getEventCount());
            auto ingestionHandle_iter = storyIngestionHandles.find(chunk->getStoryId());
            if(ingestionHandle_iter != storyIngestionHandles.end())
            {
                // Individual StoryIngestionHandle has its own mutex.
                (*ingestionHandle_iter).second->ingestChunk(chunk);
                return;
            }

            LOG_WARNING("[IngestionQueue] Orphan chunk for story {}. Storing for later processing.", chunk->getStoryId());
            orphanQueue.push_back(chunk);
            CL_PROFILE_COUNTER("grapher_orphan_queue_depth", orphanQueue.size());
        }
    }

    uint64_t recordStoryDrainComplete(StoryId const& story_id)
    {
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        uint64_t const completion_count = ++storyDrainCompletionCounts[story_id];
        LOG_INFO("[IngestionQueue] Story drain complete received. StoryId={} completionCount={}",
                 story_id,
                 completion_count);
        ingestionQueueChanged.notify_all();
        return completion_count;
    }

    uint64_t storyDrainCompletionCount(StoryId const& story_id)
    {
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        auto iter = storyDrainCompletionCounts.find(story_id);
        return iter == storyDrainCompletionCounts.end() ? 0 : iter->second;
    }

    bool waitForStoryDrainCompletions(StoryId const& story_id, uint64_t expected_count, uint64_t timeout_ms)
    {
        std::unique_lock <std::mutex> lock(ingestionQueueMutex);
        auto complete = [&]() {
            auto iter = storyDrainCompletionCounts.find(story_id);
            return iter != storyDrainCompletionCounts.end() && iter->second >= expected_count;
        };
        if(expected_count == 0)
        {
            return true;
        }
        if(timeout_ms == 0)
        {
            return complete();
        }
        return ingestionQueueChanged.wait_for(lock, std::chrono::milliseconds(timeout_ms), complete);
    }

    void drainOrphanChunks()
    {
        CL_PROFILE_REGION("grapher_orphan_queue_drain");
        if(orphanQueue.empty())
        {
            LOG_DEBUG("[IngestionQueue] Orphan chunk queue is empty. No actions taken.");
            return;
        }

        if (storyIngestionHandles.empty())
        {
            LOG_DEBUG("[IngestionQueue] has 0 storyIngestionHandles to place {} orphaned chunks", orphanQueue.size());
            return;
        }
 
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        const auto orphan_count_before = orphanQueue.size();
        for(StoryChunkDeque::iterator iter = orphanQueue.begin(); iter != orphanQueue.end();)
        {
            auto ingestionHandle_iter = storyIngestionHandles.find((*iter)->getStoryId());
            if(ingestionHandle_iter != storyIngestionHandles.end())
            {
                // Individual StoryIngestionHandle has its own mutex
                (*ingestionHandle_iter).second->ingestChunk(*iter);
                // Remove the chunk from the orphan deque and get the iterator to the next element prior to removal
                iter = orphanQueue.erase(iter);
            }
            else
            {
                LOG_DEBUG("[IngestionQueue] Orphan chunk for story {} is still orphaned.", (*iter)->getStoryId());
                ++iter;
            }
        }
            
        LOG_WARNING("[IngestionQueue] has {} orphaned chunks", orphanQueue.size());
        CL_PROFILE_COUNTER("grapher_orphan_queue_drain_chunks", orphan_count_before - orphanQueue.size());
        CL_PROFILE_COUNTER("grapher_orphan_queue_depth", orphanQueue.size());
    }

    bool is_empty() const
    {
        return (orphanQueue.empty() && storyIngestionHandles.empty());
    }

    void discardOrphanChunks(char const* reason)
    {
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        if(orphanQueue.empty())
        {
            return;
        }
        auto const orphan_count = orphanQueue.size();
        for(auto* chunk: orphanQueue)
        {
            delete chunk;
        }
        orphanQueue.clear();
        CL_PROFILE_COUNTER("grapher_orphan_queue_discarded_chunks", orphan_count);
        CL_PROFILE_COUNTER("grapher_orphan_queue_depth", orphanQueue.size());
        LOG_WARNING("[IngestionQueue] Discarded {} orphan chunks. Reason={}", orphan_count, reason);
    }

    void shutDown()
    {
        LOG_INFO("[IngestionQueue] Initiating shutdown. HandleMapSize={}, OrphanQueueSize={}"
             , storyIngestionHandles.size(), orphanQueue.size());
        // last attempt to drain orphanQueue into known ingestionHandles
        drainOrphanChunks();
        if(storyIngestionHandles.empty())
        {
            discardOrphanChunks("shutdown_without_story_handles");
        }
        // disengage all handles
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyIngestionHandles.clear();
        LOG_INFO("[IngestionQueue] Shutdown completed. All handles disengaged.");
    }

private:
    ChunkIngestionQueue(ChunkIngestionQueue const &) = delete;

    ChunkIngestionQueue &operator=(ChunkIngestionQueue const &) = delete;

    std::mutex ingestionQueueMutex;
    std::condition_variable ingestionQueueChanged;
    std::unordered_map <StoryId, StoryChunkIngestionHandle*> storyIngestionHandles;
    std::unordered_map <StoryId, uint64_t> storyDrainCompletionCounts;

    // chunks for unknown stories or late arriving chunks for closed stories will end up
    // in orphanQueue that we'll periodically try to drain into the DataStore
    std::deque <StoryChunk*> orphanQueue;
};
}

#endif
