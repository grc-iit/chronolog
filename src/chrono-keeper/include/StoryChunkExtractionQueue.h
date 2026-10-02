#ifndef CHUNK_EXTRACTION_QUEUE_H
#define CHUNK_EXTRACTION_QUEUE_H

#include <iostream>
#include <deque>
#include <mutex>
#include <string>
#include <sstream>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <StoryChunk.h>


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
        {
            std::lock_guard<std::mutex> lock(extractionQueueMutex);
            extractionDeque.push_back(story_chunk);
            LOG_DEBUG("[StoryChunkExtractionQueue] Stashed story chunk with StoryID={} and StartTime={}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime());
        }
    }

    // Takes the oldest chunk. It counts as in process until the caller reports
    // it done with chunkProcessed(); see the chrono-common copy of this queue,
    // whose idle() the grapher's destroy waits on.
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
        ++chunksInProcess;

        return story_chunk;
    }

    // A chunk ejectStoryChunk() handed out is done with.
    void chunkProcessed()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        if(chunksInProcess > 0)
        {
            --chunksInProcess;
        }
    }

    // No chunk waiting and none in process.
    bool idle()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        return extractionDeque.empty() && chunksInProcess == 0;
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
        LOG_INFO("[StoryChunkExtractionQueue] Queue has been successfully shut down and all story chunks have been "
                 "freed.");
    }

private:
    StoryChunkExtractionQueue(StoryChunkExtractionQueue const&) = delete;

    StoryChunkExtractionQueue& operator=(StoryChunkExtractionQueue const&) = delete;

    std::mutex extractionQueueMutex;
    std::deque<StoryChunk*> extractionDeque;
    // chunks ejected and not yet reported processed
    std::size_t chunksInProcess = 0;
};

} // namespace chronolog

#endif
