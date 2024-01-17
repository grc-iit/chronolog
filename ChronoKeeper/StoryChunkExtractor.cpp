#include <unistd.h>

#include <thallium.hpp>

#include "StoryChunkExtractor.h"


namespace tl = thallium;

//////////////////////////////

void chronolog::StoryChunkExtractorBase::startExtractionThreads(int stream_count)
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == RUNNING)
    {
        LOGI("[StoryChunkExtractionBase] ExtractionModule already running. Aborting start request.");
        return;
    }

    extractorState = RUNNING;
    LOGD("[StoryChunkExtractionBase] Started extraction threads.");

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed <tl::xstream> es = tl::xstream::create();
        extractionStreams.push_back(std::move(es));
    }

    for(int i = 0; i < 2 * stream_count; ++i)
    {
        tl::managed <tl::thread> th = extractionStreams[i % extractionStreams.size()]->make_thread([p = this]()
                                                                                                   { p->drainExtractionQueue(); });
        extractionThreads.push_back(std::move(th));
    }
}
//////////////////////////////

void chronolog::StoryChunkExtractorBase::shutdownExtractionThreads()
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == SHUTTING_DOWN)
    {
        LOGI("[StoryChunkExtractionBase] ExtractionModule already shutting down. Skipping shutdown request.");
        return;
    }

    extractorState = SHUTTING_DOWN;
    LOGD("[StoryChunkExtractionBase] Initiating shutdown. Queue size: {}", chunkExtractionQueue.size());

    // join threads & executionstreams while holding stateMutex
    for(auto &eth: extractionThreads)
    {
        eth->join();
    }
    LOGD("[StoryChunkExtractionBase] Extraction threads successfully shut down.");
    for(auto &es: extractionStreams)
    {
        es->join();
    }
    LOGD("[StoryChunkExtractionBase] Streams have been successfully closed.");
}

//////////////////////
chronolog::StoryChunkExtractorBase::~StoryChunkExtractorBase()
{
    LOGD("[StoryChunkExtractionBase] Destructor called. Initiating shutdown sequence.");

    shutdownExtractionThreads();

    extractionThreads.clear();
    extractionStreams.clear();
}

//////////////////////

void chronolog::StoryChunkExtractorBase::drainExtractionQueue()
{
    thallium::xstream es = thallium::xstream::self();
    // extraction threads will be running as long as the state doesn't change
    // and untill the extractionQueue is drained in shutdown mode
    while((extractorState == RUNNING) || !chunkExtractionQueue.empty())
    {
        LOGD("[StoryChunkExtractionBase] Draining queue. ES Rank: {}, ULT ID: {}, Queue Size: {}", es.get_rank()
             , thallium::thread::self_id(), chunkExtractionQueue.size());

        while(!chunkExtractionQueue.empty())
        {
            StoryChunk*storyChunk = chunkExtractionQueue.ejectStoryChunk();
            if(storyChunk == nullptr)
                //the queue might have been drained by another thread before the current thread acquired extractionQueue mutex
            {
                LOGW("[StoryChunkExtractionBase] Failed to acquire a story chunk from the queue.");
                break;
            }
            processStoryChunk(storyChunk);  // INNA: should add return type and handle the failure properly
            // free the memory or reset the startTime and return to the pool of prealocated chunks
            delete storyChunk;
        }
        sleep(30);
    }
}

