#ifndef STORY_CHUNK_INGESTION_HANDLE_H
#define STORY_CHUNK_INGESTION_HANDLE_H

#include <mutex>
#include <deque>
#include "StoryChunk.h"
//
// IngestionQueue is a funnel into the KeeperDataStore
// std::deque guarantees O(1) time for addidng elements and resizing 
// (vector of vectors implementation)

namespace chronolog
{

typedef std::deque <StoryChunk*> StoryChunkDeque;

class StoryChunkIngestionHandle
{

public:
    StoryChunkIngestionHandle(std::mutex &a_mutex, StoryChunkDeque*active, StoryChunkDeque*passive): ingestionMutex(a_mutex)
                                                                                      , activeDeque(active)
                                                                                      , passiveDeque(passive)
    {}

    ~StoryChunkIngestionHandle() = default;

    StoryChunkDeque &getActiveDeque() const
    { return *activeDeque; }

    StoryChunkDeque &getPassiveDeque() const
    { return *passiveDeque; }

    void ingestChunk(StoryChunk * chunk)
    {   // assume multiple service threads pushing chunks onto ingestionQueue
        std::lock_guard <std::mutex> lock(ingestionMutex);
        activeDeque->push_back(chunk);
    }

    void swapActiveDeque() 
    {
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

//INNA: check if atomic compare_and_swap will work here

        std::lock_guard <std::mutex> lock_guard(ingestionMutex);
        if(!passiveDeque->empty() || activeDeque->empty())
        { return; }

        StoryChunkDeque*full_deque = activeDeque;
        activeDeque = passiveDeque;
        passiveDeque = full_deque;
    }


private:
    std::mutex &ingestionMutex;
    StoryChunkDeque * activeDeque;
    StoryChunkDeque * passiveDeque;
};

}

#endif

