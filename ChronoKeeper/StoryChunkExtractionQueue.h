#ifndef CHUNK_EXTRACTION_QUEUE_H
#define CHUNK_EXTRACTION_QUEUE_H


#include <iostream>
#include <deque>
#include <mutex>
#include "log.h"

#include "chrono_common/chronolog_types.h"
#include "StoryChunk.h"

namespace chronolog
{

class StoryChunkExtractionQueue
{
public:
    StoryChunkExtractionQueue()
    {}

    ~StoryChunkExtractionQueue()
    {
        LOGD("[StoryChunkExtractionQueue] Destructor called. Initiating queue shutdown.");
        shutDown();
    }

    void stashStoryChunk(StoryChunk*story_chunk)
    {
        if(nullptr == story_chunk)
        {
            LOGW("[StoryChunkExtractionQueue] Attempted to stash a null story chunk. Ignoring.");
            return;
        }
        LOGD("[StoryChunkExtractionQueue] Stashed story chunk with StoryID={} and StartTime={}"
             , story_chunk->getStoryId(), story_chunk->getStartTime());
        {
            std::lock_guard <std::mutex> lock(extractionQueueMutex);
            extractionDeque.push_back(story_chunk);
        }
    }

    StoryChunk*ejectStoryChunk()
    {
        std::lock_guard <std::mutex> lock(extractionQueueMutex);
        if(extractionDeque.empty())
        {
            LOGD("[StoryChunkExtractionQueue] No story chunks available for ejection.");
            return nullptr;
        }
        StoryChunk*story_chunk = extractionDeque.front();
        extractionDeque.pop_front();

        return story_chunk;
    }


    int size()
    {
        std::lock_guard <std::mutex> lock(extractionQueueMutex);
        return extractionDeque.size();
    }

    bool empty()
    {
        std::lock_guard <std::mutex> lock(extractionQueueMutex);
        return extractionDeque.empty();
    }

    void shutDown()
    {
        LOGI("[StoryChunkExtractionQueue] Initiating queue shutdown. Queue size: {}", extractionDeque.size());
        if(extractionDeque.empty())
        { return; }

        //INNA: LOG a WARNING and attempt to delay shutdown until the queue is drained by the Extraction module
        // if this fails , log an ERROR .
        // free the remaining storychunks memory...
        std::lock_guard <std::mutex> lock(extractionQueueMutex);
        while(!extractionDeque.empty())
        {
            delete extractionDeque.front();
            extractionDeque.pop_front();
        }
        LOGI("[StoryChunkExtractionQueue] Queue has been successfully shut down and all story chunks have been freed.");
    }

private:
    StoryChunkExtractionQueue(StoryChunkExtractionQueue const &) = delete;

    StoryChunkExtractionQueue &operator=(StoryChunkExtractionQueue const &) = delete;

    std::mutex extractionQueueMutex;
    std::deque <StoryChunk*> extractionDeque;
};

}

#endif

