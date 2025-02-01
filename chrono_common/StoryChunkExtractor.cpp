#include <unistd.h>

#include <thallium.hpp>
#include <unistd.h>

#include "StoryChunkExtractor.h"


namespace tl = thallium;

//////////////////////////////

void chronolog::StoryChunkExtractorBase::startExtractionThreads(int stream_count)
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == RUNNING)
    {
        LOG_INFO("[StoryChunkExtractionBase] ExtractionModule already running. Aborting start request.");
        return;
    }

    extractorState = RUNNING;

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
//////////////////////////////

void chronolog::StoryChunkExtractorBase::shutdownExtractionThreads()
{
    std::lock_guard lock(extractorMutex);

    if(extractorState == SHUTTING_DOWN)
    {
        LOG_INFO("[StoryChunkExtractionBase] ExtractionModule already shutting down. Skipping shutdown request.");
        return;
    }

    extractorState = SHUTTING_DOWN;
    LOG_DEBUG("[StoryChunkExtractionBase] Initiating shutdown. Queue size: {}", chunkExtractionQueue.size());

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
chronolog::StoryChunkExtractorBase::~StoryChunkExtractorBase()
{
    LOG_DEBUG("[StoryChunkExtractionBase] Destructor called. Initiating shutdown sequence.");

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
            int ret = processStoryChunk(storyChunk);  // INNA: should add return type and handle the failure properly
            // free the memory or reset the startTime and return to the pool of prealocated chunks
            if(ret == CL_SUCCESS)
            {
                LOG_DEBUG("[StoryChunkExtractionBase] StoryChunk processed successfully. ES Rank: {}, ULT ID: {}"
                          , es.get_rank(), thallium::thread::self_id());
                delete storyChunk;
            }
            else
            {
                LOG_ERROR("[StoryChunkExtractionBase] Failed to process a story chunk, Error Code: {}. ES Rank: {}, "
                          "ULT ID: {}", ret, es.get_rank(), thallium::thread::self_id());
                LOG_ERROR(
                        "[StoryChunkExtractionBase] Stashing the story chunk for later processing... ES Rank: {}, ULT "
                        "ID: {}", es.get_rank(), thallium::thread::self_id());
                chunkExtractionQueue.stashStoryChunk(storyChunk);
            }
        }
        sleep(3);
    }
}
