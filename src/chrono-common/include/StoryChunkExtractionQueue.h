#ifndef CHUNK_EXTRACTION_QUEUE_H
#define CHUNK_EXTRACTION_QUEUE_H

#include <iostream>
#include <deque>
#include <mutex>

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
        LOG_TRACE("[StoryChunkExtractionQueue] Destructor called.");
        shutDown();
    }

    void stashStoryChunk(StoryChunk* story_chunk)
    {
        if(nullptr == story_chunk)
        {
            return;
        }
        LOG_DEBUG("[StoryChunkExtractionQueue] Stashed story chunk with StoryID={} and StartTime={}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime());
        {
            std::lock_guard<std::mutex> lock(extractionQueueMutex);
            extractionDeque.push_back(story_chunk);
        }
    }

    void stashStoryChunks(std::vector<StoryChunk*> story_chunks)
    {
        if(story_chunks.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        for(auto const& chunk_ptr: story_chunks)
        {
            if(nullptr != chunk_ptr)
            {
                LOG_DEBUG("[StoryChunkExtractionQueue] Stashed story chunk with StoryID={} and StartTime={}",
                          chunk_ptr->getStoryId(),
                          chunk_ptr->getStartTime());
                extractionDeque.push_back(chunk_ptr);
            }
        }
        story_chunks.clear();
    }

    // Takes the oldest chunk. It counts as in process until the caller reports
    // it done with chunkProcessed(), so idle() stays false while the chunk is
    // being written, not only while it waits here.
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

    // A chunk ejectStoryChunk() handed out is done with: written, handed back,
    // stashed again or deleted.
    void chunkProcessed()
    {
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        if(chunksInProcess > 0)
        {
            --chunksInProcess;
        }
    }

    // No chunk waiting and none in process. The grapher waits for this before it
    // deletes a destroyed story's files: a chunk taken off the queue can still be
    // mid-write, and its file only appears under its name once the write is
    // complete, so a delete run before then would miss it.
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
        std::lock_guard<std::mutex> lock(extractionQueueMutex);
        LOG_INFO("[StoryChunkExtractionQueue] Initiating queue shutdown. Queue size: {}", extractionDeque.size());
        // we should never get to this point as many attempts were made by now to process the chunk
        // process the chunk or put on the extractors outage buffer
        // this is just an extra safety measure...
        // to free the remaining storychunks memory...
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
