#ifndef CHUNK_EXTRACTION_QUEUE_H
#define CHUNK_EXTRACTION_QUEUE_H


#include <iostream>
#include <deque>
#include <mutex>

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
    { shutDown(); }

    void stashStoryChunk(StoryChunk*story_chunk)
    {
        if(nullptr == story_chunk)
        { return; }

        std::cout << "ExtractionQueue::stashStoryChunk: storyId{" << story_chunk->getStoryId() << "}{"
                  << story_chunk->getStartTime() << std::endl;
        {
            std::lock_guard <std::mutex> lock(extractionQueueMutex);
            extractionDeque.push_back(story_chunk);
        }
    }

    StoryChunk*ejectStoryChunk()
    {
        std::lock_guard <std::mutex> lock(extractionQueueMutex);
        if(extractionDeque.empty())
        { return nullptr; }
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
        std::cout << "ExtractionQueue: shutdown : extractionQueue.size=" << extractionDeque.size() << std::endl;
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
    }

private:
    StoryChunkExtractionQueue(StoryChunkExtractionQueue const &) = delete;

    StoryChunkExtractionQueue &operator=(StoryChunkExtractionQueue const &) = delete;

    std::mutex extractionQueueMutex;
    std::deque <StoryChunk*> extractionDeque;
};

}

#endif

