#ifndef CHUNK_EXTRACTION_QUEUE_H
#define CHUNK_EXTRACTION_QUEUE_H

#include <iostream>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include <chrono_monitor.h>

#include "chronolog_types.h"
#include "StoryChunk.h"

namespace chronolog
{

class StoryChunkExtractionQueue
{
public:
    StoryChunkExtractionQueue() {}

    ~StoryChunkExtractionQueue()
    {
        LOG_DEBUG("[StoryChunkExtractionQueue] Destructor called. Initiating queue shutdown.");
        shutDown();
    }

    void stashStoryChunk(StoryChunk* story_chunk)
    {
        if(nullptr == story_chunk)
        {
            LOG_WARNING("[StoryChunkExtractionQueue] Attempted to stash a null story chunk. Ignoring.");
            return;
        }
        LOG_DEBUG("[StoryChunkExtractionQueue] Stashed story chunk with StoryID={} and StartTime={}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime());
        {
            std::lock_guard<std::mutex> lock(extractionQueueMutex);
            extractionDeque.push_back(story_chunk);
            ++queuedByStory[story_chunk->getStoryId()];
        }
        extractionQueueChanged.notify_all();
    }

    StoryChunk* ejectStoryChunk()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        if(extractionDeque.empty())
        {
            LOG_DEBUG("[StoryChunkExtractionQueue] No story chunks available for ejection.");
            return nullptr;
        }
        StoryChunk* story_chunk = extractionDeque.front();
        extractionDeque.pop_front();
        decrementCounter(queuedByStory, story_chunk->getStoryId());
        ++inFlightByStory[story_chunk->getStoryId()];

        return story_chunk;
    }

    void completeStoryChunk(StoryId const& story_id)
    {
        {
            std::lock_guard<std::mutex> lock(extractionQueueMutex);
            decrementCounter(inFlightByStory, story_id);
        }
        extractionQueueChanged.notify_all();
    }

    bool waitForStoryDrain(StoryId const& story_id, uint64_t timeout_ms)
    {
        std::unique_lock<std::mutex> lock(extractionQueueMutex);
        auto drained = [&]() {
            return countForStory(queuedByStory, story_id) == 0 && countForStory(inFlightByStory, story_id) == 0;
        };
        if(timeout_ms == 0)
        {
            return drained();
        }
        return extractionQueueChanged.wait_for(lock, std::chrono::milliseconds(timeout_ms), drained);
    }

    size_t queuedStoryChunkCount(StoryId const& story_id)
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        return countForStory(queuedByStory, story_id);
    }

    size_t inFlightStoryChunkCount(StoryId const& story_id)
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        return countForStory(inFlightByStory, story_id);
    }

    int size()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        return extractionDeque.size();
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        return extractionDeque.empty();
    }

    void shutDown()
    {
        LOG_INFO("[StoryChunkExtractionQueue] Initiating queue shutdown. Queue size: {}", extractionDeque.size());
        if(extractionDeque.empty())
        {
            return;
        }

        //INNA: LOG a WARNING and attempt to delay shutdown until the queue is drained by the Extraction module
        // if this fails , log an ERROR .
        // free the remaining storychunks memory...
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        while(!extractionDeque.empty())
        {
            delete extractionDeque.front();
            extractionDeque.pop_front();
        }
        queuedByStory.clear();
        inFlightByStory.clear();
        LOG_INFO("[StoryChunkExtractionQueue] Queue has been successfully shut down and all story chunks have been "
                 "freed.");
        extractionQueueChanged.notify_all();
    }

private:
    StoryChunkExtractionQueue(StoryChunkExtractionQueue const&) = delete;

    StoryChunkExtractionQueue& operator=(StoryChunkExtractionQueue const&) = delete;

    static size_t countForStory(std::unordered_map<StoryId, size_t> const& counters, StoryId const& story_id)
    {
        auto iter = counters.find(story_id);
        return iter == counters.end() ? 0 : iter->second;
    }

    static void decrementCounter(std::unordered_map<StoryId, size_t>& counters, StoryId const& story_id)
    {
        auto iter = counters.find(story_id);
        if(iter == counters.end())
        {
            return;
        }
        if(iter->second <= 1)
        {
            counters.erase(iter);
        }
        else
        {
            --iter->second;
        }
    }

    std::mutex extractionQueueMutex;
    std::condition_variable extractionQueueChanged;
    std::deque<StoryChunk*> extractionDeque;
    std::unordered_map<StoryId, size_t> queuedByStory;
    std::unordered_map<StoryId, size_t> inFlightByStory;
};

} // namespace chronolog

#endif
