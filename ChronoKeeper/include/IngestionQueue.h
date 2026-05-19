#ifndef INGESTION_QUEUE_H
#define INGESTION_QUEUE_H

#include <algorithm>
#include <iostream>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <string>
#include <utility>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <chronolog_profile.h>

#include "StoryIngestionHandle.h"
#include "KeeperAppendStats.h"

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
        {
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
            ingestionQueueMutex.lock();
        }
        std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
        storyIngestionHandles.emplace(std::pair<StoryId, StoryIngestionHandle*>(story_id, ingestion_handle));
        CL_PROFILE_COUNTER("keeper_ingestion_handle_count", storyIngestionHandles.size());
        LOG_DEBUG("[IngestionQueue] Added handle for StoryID={}: HandleAddress={}, StoryIngestionHandles={}, "
                  "HandleMapSize={}",
                  story_id,
                  static_cast<void*>(ingestion_handle),
                  reinterpret_cast<void*>(&storyIngestionHandles),
                  storyIngestionHandles.size());
    }

    void removeIngestionHandle(StoryId const& story_id)
    {
        {
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
            ingestionQueueMutex.lock();
        }
        std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
        if(storyIngestionHandles.erase(story_id))
        {
            CL_PROFILE_COUNTER("keeper_ingestion_handle_count", storyIngestionHandles.size());
            LOG_DEBUG("[IngestionQueue] Removed handle for StoryID={}. Current handle MapSize={}",
                      story_id,
                      storyIngestionHandles.size());
        }
        else
        {
            LOG_WARNING("[IngestionQueue] Tried to remove non-existent handle for StoryID={}.", story_id);
        }
    }

    void ingestLogEvent(LogEvent const& event)
    {
        ingestLogEventImpl(event);
    }

    void ingestLogEvent(LogEvent&& event)
    {
        ingestLogEventImpl(std::move(event));
    }

    void ingestLogEvents(std::vector<LogEvent>&& events)
    {
        if(events.empty())
        {
            return;
        }
        CL_PROFILE_REGION("keeper_ingest_event_batch");
        StoryId const story_id = events.front().storyId;
        bool const same_story = std::all_of(events.begin(), events.end(), [story_id](LogEvent const& event) {
            return event.storyId == story_id;
        });
        if(!same_story)
        {
            for(auto& event: events)
            {
                ingestLogEvent(std::move(event));
            }
            return;
        }

        uint64_t const stats_start_ns = KeeperAppendStats::nowNs();
        uint64_t const lookup_start_ns = KeeperAppendStats::nowNs();
        auto ingestionHandle_iter = storyIngestionHandles.find(story_id);
        uint64_t const lookup_duration_ns = KeeperAppendStats::nowNs() - lookup_start_ns;
        if(ingestionHandle_iter == storyIngestionHandles.end())
        {
            LOG_WARNING("[IngestionQueue] Orphan event batch for story {}. Storing {} events for later processing.",
                        story_id,
                        events.size());
            {
                CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
                ingestionQueueMutex.lock();
            }
            std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
            CL_PROFILE_REGION("keeper_orphan_queue_push_batch");
            for(auto& event: events)
            {
                orphanEventQueue.push_back(std::move(event));
            }
            CL_PROFILE_COUNTER("keeper_orphan_queue_depth", orphanEventQueue.size());
            KeeperAppendStats::instance().recordIngestionQueueBatch(KeeperAppendStats::nowNs() - stats_start_ns,
                                                                    lookup_duration_ns,
                                                                    events.size(),
                                                                    true);
            return;
        }

        std::size_t const event_count = events.size();
        (*ingestionHandle_iter).second->ingestEvents(std::move(events));
        KeeperAppendStats::instance().recordIngestionQueueBatch(KeeperAppendStats::nowNs() - stats_start_ns,
                                                                lookup_duration_ns,
                                                                event_count,
                                                                false);
    }

private:
    template <typename EventT>
    void ingestLogEventImpl(EventT&& event)
    {
        CL_PROFILE_REGION("keeper_ingest_event");
        const uint64_t stats_start_ns = KeeperAppendStats::nowNs();
        LOG_DEBUG("[IngestionQueue] Received event for StoryID={}: EventTime={} ClientID={} EventIndex={} "
                  "PayloadBytes={}",
                  event.storyId,
                  event.eventTime,
                  event.clientId,
                  event.eventIndex,
                  event.logRecord.size());
        const uint64_t lookup_start_ns = KeeperAppendStats::nowNs();
        auto ingestionHandle_iter = storyIngestionHandles.find(event.storyId);
        const uint64_t lookup_duration_ns = KeeperAppendStats::nowNs() - lookup_start_ns;
        if(ingestionHandle_iter == storyIngestionHandles.end())
        {
            LOG_WARNING("[IngestionQueue] Orphan event for story {}. Storing for later processing.", event.storyId);
            {
                CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
                ingestionQueueMutex.lock();
            }
            std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
            CL_PROFILE_REGION("keeper_orphan_queue_push");
            orphanEventQueue.push_back(std::forward<EventT>(event));
            CL_PROFILE_COUNTER("keeper_orphan_queue_depth", orphanEventQueue.size());
            KeeperAppendStats::instance().recordIngestionQueue(KeeperAppendStats::nowNs() - stats_start_ns,
                                                               lookup_duration_ns,
                                                               true);
        }
        else
        {
            //individual StoryIngestionHandle has its own mutex
            (*ingestionHandle_iter).second->ingestEvent(std::forward<EventT>(event));
            KeeperAppendStats::instance().recordIngestionQueue(KeeperAppendStats::nowNs() - stats_start_ns,
                                                               lookup_duration_ns,
                                                               false);
        }
    }

public:

    void drainOrphanEvents()
    {
        CL_PROFILE_REGION("keeper_orphan_queue_drain");
        if(orphanEventQueue.empty())
        {
            LOG_DEBUG("[IngestionQueue] Orphan event queue is empty. No actions taken.");
            return;
        }
        {
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
            ingestionQueueMutex.lock();
        }
        std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
        const auto orphan_count_before = orphanEventQueue.size();
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
        CL_PROFILE_COUNTER("keeper_orphan_queue_drain_events", orphan_count_before - orphanEventQueue.size());
        CL_PROFILE_COUNTER("keeper_orphan_queue_depth", orphanEventQueue.size());
        LOG_DEBUG("[IngestionQueue] Drained {} orphan events into known handles.", orphanEventQueue.size());
    }

    bool is_empty() const { return (orphanEventQueue.empty() && storyIngestionHandles.empty()); }

    void shutDown()
    {
        LOG_INFO("[IngestionQueue] Initiating shutdown. HandleMapSize={}, Orphan EventQueueSize={}",
                 storyIngestionHandles.size(),
                 orphanEventQueue.size());
        // last attempt to drain orphanEventQueue into known ingestionHandles
        drainOrphanEvents();
        // disengage all handles
        {
            CL_PROFILE_REGION("keeper_ingestion_queue_lock_wait");
            ingestionQueueMutex.lock();
        }
        std::lock_guard<std::mutex> lock(ingestionQueueMutex, std::adopt_lock);
        CL_PROFILE_REGION("keeper_ingestion_queue_lock_hold");
        storyIngestionHandles.clear();
        LOG_INFO("[IngestionQueue] Shutdown completed. All handles disengaged.");
    }

private:
    IngestionQueue(IngestionQueue const&) = delete;

    IngestionQueue& operator=(IngestionQueue const&) = delete;

    std::mutex ingestionQueueMutex;
    std::unordered_map<StoryId, StoryIngestionHandle*> storyIngestionHandles;

    // events for unknown stories or late events for closed stories will end up
    // in orphanEventQueue that we'll periodically try to drain into the DataStore
    std::deque<LogEvent> orphanEventQueue;

    //Timer to triger periodic attempt to drain orphanEventQueue and collect/log statistics
};
} // namespace chronolog

#endif
