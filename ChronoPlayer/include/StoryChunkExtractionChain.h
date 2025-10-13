#ifndef STORY_CHUNK_EXTRACTION_CHAIN_H
#define STORY_CHUNK_EXTRACTION_CHAIN_H

#include <iostream>

//#include <deque>
//#include <vector>
//#include <mutex>
#include <atomic>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "StoryChunkExtractionQueue.h"
#include "chronolog_errcode.h"

namespace tl = thallium;

namespace chronolog
{
template <typename T>
class StoryChunkExtractionChain
{
    enum State
    {
        UNKNOWN = 0, RUNNING = 1, //  active extraction threads
        SHUTTING_DOWN = 2 // Shutting down extraction threads
    };

public:
    StoryChunkExtractionChain()
    : state(UNKNOWN)
    {}

    StoryChunkExtractionQueue & getExtractionQueue()
    {
        return chunkExtractionQueue;
    }

    bool is_running() const
    { return (state == RUNNING); }

    bool is_shutting_down() const
    { return (state == SHUTTING_DOWN); }

    void drainExtractionQueue()
{
    thallium::xstream es = thallium::xstream::self();
    // extraction threads will be running as long as the state doesn't change
    // and untill the extractionQueue is drained in shutdown mode
    LOG_DEBUG("[StoryChunkExtractionBase] Draining queue. ES Rank: {}, ULT ID: {}, Queue Size: {}", es.get_rank()
                  , thallium::thread::self_id(), chunkExtractionQueue.size());

    while(!chunkExtractionQueue.empty())
    {
            StoryChunk* storyChunk = chunkExtractionQueue.ejectStoryChunk();
            if(storyChunk == nullptr)
                //the queue might have been drained by another thread before the current thread acquired extractionQueue mutex
            {
                LOG_WARNING("[StoryChunkExtractionBase] Failed to acquire a story chunk from the queue.");
                break;
            }
            extractorChain.processStoryChunk(storyChunk);  // INNA: should add return type and handle the failure properly
            // free the memory or reset the startTime and return to the pool of prealocated chunks
            {
                LOG_DEBUG("[StoryChunkExtractionBase] StoryChunk processed successfully. ES Rank: {}, ULT ID: {}"
                          , es.get_rank(), thallium::thread::self_id());
                delete storyChunk;
            }
    }
    sleep(1);
}

    void startExtractionThreads(int stream_count)
{

    if(state == RUNNING)
    {
        LOG_INFO("[StoryChunkExtractionBase] ExtractionModule already running. Aborting start request.");
        return;
    }

    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed <tl::xstream> es = tl::xstream::create();
        extractionStreams.push_back(std::move(es));
    }

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed <tl::thread> th = extractionStreams[i % extractionStreams.size()]->make_thread([p = this]()
                                                                                                   { p->drainExtractionQueue(); });
        extractionThreads.push_back(std::move(th));
    }
    LOG_DEBUG("[StoryChunkExtractionBase] Started extraction threads.");
}

    void shutdownExtractionThreads()
{

    if(state == SHUTTING_DOWN)
    {
        LOG_INFO("[StoryChunkExtractionBase] ExtractionModule already shutting down. Skipping shutdown request.");
        return;
    }

    state = SHUTTING_DOWN;
    LOG_DEBUG("[StoryChunkExtractionBase] Initiating shutdown. Queue size: {}", chunkExtractionQueue.size());

    drainExtractionQueue();

    // join threads & executionstreams while holding stateMutex
    for(auto &eth: extractionThreads)
    {
        eth->join();
    }
    LOG_DEBUG("[StoryChunkExtractionBase] Extraction threads successfully shut down.");
    for(auto &es: extractionStreams)
    {
        es->join();
    }
    LOG_DEBUG("[StoryChunkExtractionBase] Streams have been successfully closed.");
}

//////////////////////
~StoryChunkExtractionChain()
{
    LOG_DEBUG("[StoryChunkExtractionBase] Destructor called. Initiating shutdown sequence.");

    shutdownExtractionThreads();

    extractionThreads.clear();
    extractionStreams.clear();
}

//////////////////////

private:
    StoryChunkExtractionChain(StoryChunkExtractionChain const &) = delete;

    StoryChunkExtractionChain & operator=(StoryChunkExtractionChain const &) = delete;
    
//    void startExtractionThreads(int);
//    void shutdownExtractionThreads();

    std::atomic<State> state; 
    //std::mutex extractorMutex;
    StoryChunkExtractionQueue chunkExtractionQueue;

    typedef T extractorChain;

    std::vector <tl::managed <tl::xstream>> extractionStreams;
    std::vector <tl::managed <tl::thread>> extractionThreads;
};


class ExtractionTail
{
public:
    ExtractionTail() = default;
    ~ExtractionTail() = default;
    
    StoryChunk const* processStoryChunk( StoryChunk const* chunk)
    { return chunk;  }
};
}

#endif

