#ifndef CHUNK_INGESTION_QUEUE_H
#define CHUNK_INGESTION_QUEUE_H


#include <iostream>
#include <deque>
#include <unordered_map>
#include <mutex>
#include "chrono_monitor.h"

#include "chronolog_types.h"
#include "StoryChunk.h"
#include "StoryChunkIngestionHandle.h"

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
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyIngestionHandles.emplace(std::pair <StoryId, StoryChunkIngestionHandle*>(story_id, ingestion_handle));
        LOG_DEBUG("[IngestionQueue] Added handle for StoryID={}: HandleAddress={}, StoryIngestionHandles={}, HandleMapSize={}"
             , story_id, static_cast<void*>(ingestion_handle), reinterpret_cast<void*>(&storyIngestionHandles)
             , storyIngestionHandles.size());
    }

    void removeStoryIngestionHandle(StoryId const &story_id)
    {
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        if(storyIngestionHandles.erase(story_id))
        {
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
        LOG_DEBUG("[IngestionQueue] has {} StoryHandles; Received chunk for StoryID={} startTime {} eventCount{}", storyIngestionHandles.size(),
                                chunk->getStoryId(), chunk->getStartTime(), chunk->getEventCount());
        auto ingestionHandle_iter = storyIngestionHandles.find(chunk->getStoryId());
        if(ingestionHandle_iter == storyIngestionHandles.end())
        {
            LOG_WARNING("[IngestionQueue] Orphan chunk for story {}. Storing for later processing.", chunk->getStoryId());
            std::lock_guard <std::mutex> lock(ingestionQueueMutex);
            orphanQueue.push_back(chunk);
        }
        else
        {
            //individual StoryIngestionHandle has its own mutex
            (*ingestionHandle_iter).second->ingestChunk(chunk);
        }
    }

    void drainOrphanChunks()
    {
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
    }

    bool is_empty() const
    {
        return (orphanQueue.empty() && storyIngestionHandles.empty());
    }

    void shutDown()
    {
        LOG_INFO("[IngestionQueue] Initiating shutdown. HandleMapSize={}, OrphanQueueSize={}"
             , storyIngestionHandles.size(), orphanQueue.size());
        // last attempt to drain orphanQueue into known ingestionHandles
        drainOrphanChunks();
        // disengage all handles
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyIngestionHandles.clear();
        LOG_INFO("[IngestionQueue] Shutdown completed. All handles disengaged.");
    }

private:
    ChunkIngestionQueue(ChunkIngestionQueue const &) = delete;

    ChunkIngestionQueue &operator=(ChunkIngestionQueue const &) = delete;

    std::mutex ingestionQueueMutex;
    std::unordered_map <StoryId, StoryChunkIngestionHandle*> storyIngestionHandles;

    // chunks for unknown stories or late arriving chunks for closed stories will end up
    // in orphanQueue that we'll periodically try to drain into the DataStore
    std::deque <StoryChunk*> orphanQueue;
};
}

#endif

