#ifndef INGESTION_QUEUE_H
#define INGESTION_QUEUE_H


#include <iostream>
#include <deque>
#include <unordered_map>
#include <mutex>
#include "chrono_monitor.h"

#include "chronolog_types.h"
#include "StoryIngestionHandle.h"

//
// IngestionQueue is a funnel into the MemoryDataStore
// std::deque guarantees O(1) time for addidng elements and resizing
// (vector of vectors implementation)

namespace chronolog
{

typedef std::deque <LogEvent> EventDeque;

class IngestionQueue
{
public:
    IngestionQueue()
    {}

    ~IngestionQueue()
    { shutDown(); }

    void addStoryIngestionHandle(StoryId const &story_id, StoryIngestionHandle*ingestion_handle)
    {
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyIngestionHandles.emplace(std::pair <StoryId, StoryIngestionHandle*>(story_id, ingestion_handle));
        LOG_DEBUG("[IngestionQueue] Added handle for StoryID={}: HandleAddress={}, StoryIngestionHandles={}, HandleMapSize={}"
             , story_id, static_cast<void*>(ingestion_handle), reinterpret_cast<void*>(&storyIngestionHandles)
             , storyIngestionHandles.size());
    }

    void removeIngestionHandle(StoryId const &story_id)
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

    void ingestLogEvent(LogEvent const &event)
    {
        std::stringstream ss;
        ss << event;
        LOG_DEBUG("[IngestionQueue] Received event for StoryID={}: Event Details={}, HandleMapSize={}", event.storyId
             , ss.str(), storyIngestionHandles.size());
        auto ingestionHandle_iter = storyIngestionHandles.find(event.storyId);
        if(ingestionHandle_iter == storyIngestionHandles.end())
        {
            LOG_WARNING("[IngestionQueue] Orphan event for story {}. Storing for later processing.", event.storyId);
            std::lock_guard <std::mutex> lock(ingestionQueueMutex);
            orphanEventQueue.push_back(event);
        }
        else
        {
            //individual StoryIngestionHandle has its own mutex
            (*ingestionHandle_iter).second->ingestEvent(event);
        }
    }

    void drainOrphanEvents()
    {
        if(orphanEventQueue.empty())
        {
            LOG_DEBUG("[IngestionQueue] Orphan event queue is empty. No actions taken.");
            return;
        }
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
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
        return (orphanEventQueue.empty() && storyIngestionHandles.empty());
    }

    void shutDown()
    {
        LOG_INFO("[IngestionQueue] Initiating shutdown. HandleMapSize={}, Orphan EventQueueSize={}"
             , storyIngestionHandles.size(), orphanEventQueue.size());
        // last attempt to drain orphanEventQueue into known ingestionHandles
        drainOrphanEvents();
        // disengage all handles
        std::lock_guard <std::mutex> lock(ingestionQueueMutex);
        storyIngestionHandles.clear();
        LOG_INFO("[IngestionQueue] Shutdown completed. All handles disengaged.");
    }

private:
    IngestionQueue(IngestionQueue const &) = delete;

    IngestionQueue &operator=(IngestionQueue const &) = delete;

    std::mutex ingestionQueueMutex;
    std::unordered_map <StoryId, StoryIngestionHandle*> storyIngestionHandles;

    // events for unknown stories or late events for closed stories will end up
    // in orphanEventQueue that we'll periodically try to drain into the DataStore
    std::deque <LogEvent> orphanEventQueue;

    //Timer to triger periodic attempt to drain orphanEventQueue and collect/log statistics
};
}

#endif

