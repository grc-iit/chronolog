#ifndef STORY_CHUNK_EXTRACTION_MODULE_H
#define STORY_CHUNK_EXTRACTION_MODULE_H

#include <iostream>
#include <type_traits>

#include <unistd.h>
#include <atomic>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>
#include <ExtractionModuleConfiguration.h>

namespace tl = thallium;


namespace chronolog
{

template <typename T>
class StoryChunkExtractionModule
{
    enum State
    {
        UNKNOWN = 0,
        INITIALIZED = 1,  // ExtractionChain initialized and ready
        RUNNING = 2,      //  active extraction threads
        SHUTTING_DOWN = 3 // Shutting down extraction threads
    };

public:
    StoryChunkExtractionModule(int extraction_stream_count = 2)
        : state(UNKNOWN)
        , stream_count(extraction_stream_count)
    {}

    StoryChunkExtractionModule(ServiceId const& recording_service_id,
                               ExtractionModuleConfiguration const& configuration)
        : state(UNKNOWN)
        , stream_count(2)
    {
        initialize(recording_service_id, configuration);
    }

    int initialize(ServiceId const& recording_service_id, ExtractionModuleConfiguration const& configuration)
    {
        //TODO: move extraction engine instantiation here
        // then move Extraction Chain instantiation here as well
        stream_count = configuration.extraction_stream_count;

        //TODO:      theExtractionChain.activate(recording_service_id, configuration);

        if(!theExtractionChain.is_active_chain())
        {
            return CL_ERR_INVALID_CONF;
        }

        state = INITIALIZED;
        return CL_SUCCESS;
    }

    int initialize(int extraction_stream_count)
    {
        stream_count = extraction_stream_count;

        if(!theExtractionChain.is_active_chain())
        {
            return CL_ERR_INVALID_CONF;
        }

        state = INITIALIZED;
        return CL_SUCCESS;
    }

    StoryChunkExtractionQueue& getExtractionQueue() { return chunkExtractionQueue; }

    T& getExtractionChain() { return theExtractionChain; }

    bool is_initialized() const { return (state == INITIALIZED); }

    bool is_running() const { return (state == RUNNING); }

    bool is_shutting_down() const { return (state == SHUTTING_DOWN); }

    void drainExtractionQueue()
    {
        thallium::xstream es = thallium::xstream::self();
        // extraction threads will be running as long as the state == RUNNING
        // and until the extractionQueue is drained in when state == SHUTTTING_DOWN

        LOG_DEBUG("[StoryChunkExtractionModule] DrainExtractionQueue called by stream rank={}, tl::thread_id={} "
                  "extractionQueue size {} ",
                  es.get_rank(),
                  thallium::thread::self_id(),
                  chunkExtractionQueue.size());

        // while state== RUNNING keep draining the queue
        // or waiting if the queue is empty

        int extraction_result;

        while(state == RUNNING)
        {
            if(!chunkExtractionQueue.empty())
            {
                // chunkExtractionQueue has internal mutex protecting its integrity
                StoryChunk* story_chunk = chunkExtractionQueue.ejectStoryChunk();

                // the queue might have been drained by another thread before the current thread acquired extractionQueue mutex
                // in this case the nullptr is returned..
                if(story_chunk != nullptr)
                {
                    LOG_DEBUG("[StoryChunkExtractionModule] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} "
                              "eventCount {}",
                              thallium::thread::self_id(),
                              story_chunk->getStoryId(),
                              story_chunk->getChronicleName(),
                              story_chunk->getStoryName(),
                              story_chunk->getStartTime(),
                              story_chunk->getEndTime(),
                              story_chunk->getEventCount());

                    extraction_result = theExtractionChain.process_chunk(story_chunk);

                    if(CL_SUCCESS == extraction_result)
                    { // free the story_chunk memory or
                        // return it to the pool of prealocated chunks
                        delete story_chunk;
                    }
                    else
                    { //return the story_chunk to the extractionQueue and try again later
                        chunkExtractionQueue.stashStoryChunk(story_chunk);
                    }
                }
            }
            else
            {
                usleep(1000); // 1 ms: allow OS thread to sleep when queue is idle
            }
        }
    }

    void startExtraction()
    {

        if(state == RUNNING)
        {
            LOG_INFO("[StoryChunkExtractionModule] ExtractionModule already running; ignoring start request.");
            return;
        }

        if(state != INITIALIZED)
        {
            LOG_INFO("[StoryChunkExtractionModule] Can't start extraction: ExtractionModule is either not initialized "
                     "or is shutting down");
            return;
        }

        state = RUNNING;

        for(int i = 0; i < stream_count; ++i)
        {
            tl::managed<tl::xstream> es = tl::xstream::create();
            extractionStreams.push_back(std::move(es));
        }

        for(int i = 0; i < stream_count; ++i)
        {
            tl::managed<tl::thread> th = extractionStreams[i % extractionStreams.size()]->make_thread(
                    [p = this]() { p->drainExtractionQueue(); });
            extractionThreads.push_back(std::move(th));
        }
        LOG_DEBUG("[StoryChunkExtractionModule] Started extraction threads.");
    }

    void shutdownExtraction()
    {

        if(state == SHUTTING_DOWN)
        {
            LOG_INFO("[StoryChunkExtractionModule] ExtractionModule already shutting down. Ignoring shutdown request.");
            return;
        }

        state = SHUTTING_DOWN;

        // make sure extractionQueue is drained before the extraction streams are shut down
        LOG_INFO("[StoryChunkExtractionModule] Initiating shutdown: extractionQueue size {}",
                 chunkExtractionQueue.size());

        // make the best to drain the extraction queue before exiting
        while(!chunkExtractionQueue.empty())
        {
            // chunkExtractionQueue has internal mutex protecting its integrity
            StoryChunk* story_chunk = chunkExtractionQueue.ejectStoryChunk();

            // the queue might have been drained by another thread before the current thread acquired extractionQueue mutex
            // in this case the nullptr is returned..
            if(story_chunk == nullptr)
            {
                continue;
            }

            LOG_DEBUG("[StoryChunkExtractionModule] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} "
                      "eventCount {}",
                      thallium::thread::self_id(),
                      story_chunk->getStoryId(),
                      story_chunk->getChronicleName(),
                      story_chunk->getStoryName(),
                      story_chunk->getStartTime(),
                      story_chunk->getEndTime(),
                      story_chunk->getEventCount());

            //try to extract the chunk 5 times before giving up
            int extraction_result = CL_ERR_UNKNOWN;
            int tries = 0;
            while(CL_SUCCESS != extraction_result || (tries < 5))
            {
                tries++;
                extraction_result = theExtractionChain.process_chunk(story_chunk);
            }

            if(CL_SUCCESS == extraction_result)
            {
                LOG_INFO("[StoryChunkExtractionModule] extracted chunk StoryId={} {}-{} {}-{}",
                         story_chunk->getStoryId(),
                         story_chunk->getChronicleName(),
                         story_chunk->getStoryName(),
                         story_chunk->getStartTime(),
                         story_chunk->getEndTime());
            }
            else
            {
                LOG_ERROR("[StoryChunkExtractionModule] failed to extract chunk StoryId={} {}-{} {}-{}",
                          story_chunk->getStoryId(),
                          story_chunk->getChronicleName(),
                          story_chunk->getStoryName(),
                          story_chunk->getStartTime(),
                          story_chunk->getEndTime());
            }

            delete story_chunk;
        }
        // join and stop threads & executionstreams
        for(auto& eth: extractionThreads) { eth->join(); }
        LOG_DEBUG("[StoryChunkExtractionModule] Extraction threads have been successfully shut down.");
        for(auto& es: extractionStreams) { es->join(); }
        LOG_DEBUG("[StoryChunkExtractionModule] Streams have been successfully closed.");
    }

    //////////////////////
    ~StoryChunkExtractionModule()
    {
        LOG_DEBUG("[StoryChunkExtractionModule] Destructor called. Initiating shutdown: extractionQueue size {}",
                  chunkExtractionQueue.size());

        shutdownExtraction();

        extractionThreads.clear();
        extractionStreams.clear();
    }

    //////////////////////

private:
    StoryChunkExtractionModule(StoryChunkExtractionModule const&) = delete;

    StoryChunkExtractionModule& operator=(StoryChunkExtractionModule const&) = delete;

    std::atomic<State> state;
    StoryChunkExtractionQueue chunkExtractionQueue;

    int stream_count;
    std::vector<tl::managed<tl::xstream>> extractionStreams;
    std::vector<tl::managed<tl::thread>> extractionThreads;

    T theExtractionChain;
};


} // namespace chronolog

#endif
