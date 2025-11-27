#ifndef STORY_CHUNK_EXTRACTION_CHAIN_H
#define STORY_CHUNK_EXTRACTION_CHAIN_H

#include <iostream>
#include <type_traits>

#include <atomic>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>

namespace tl = thallium;

namespace chronolog
{


// Recursive ExtractorChain Definition
template <typename T, typename... Args>
class ExtractorChain
{
public:
    ExtractorChain ( const T* extractor, const Args*... rest)
        : the_extractor(*extractor)
        , the_rest(rest...)
    { }

    ExtractorChain(const T& extractor, const Args&... rest)
        : the_extractor(extractor)
        , the_rest(rest...)
    {}

    constexpr size_t size() const { return 1 + the_rest.size(); }

    const T& extractor() const { return the_extractor; }

    const ExtractorChain<Args...>& get_rest() const { return the_rest; }

    int process_chunk(StoryChunk* some_chunk)
    {
        the_extractor.process_chunk(some_chunk);
        return the_rest.process_chunk(some_chunk);
    }

private:
    T the_extractor;
    ExtractorChain<Args...> the_rest;
};

// Base Case ExtractorChain
template <typename T>
class ExtractorChain<T>
{
public:
    ExtractorChain(const T& an_extractor)
        : the_extractor(an_extractor)
    {}

    constexpr size_t size() const { return 1; }

    const T& extractor() const { return the_extractor; }

    int process_chunk(StoryChunk* some_chunk) { return the_extractor.process_chunk(some_chunk); }

private:
    T the_extractor;
};

///////////////////////////

template <typename... Args>
class StoryChunkExtractionModule
{
    enum State
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active extraction threads
        SHUTTING_DOWN = 2 // Shutting down extraction threads
    };

public:
    StoryChunkExtractionModule(const Args&... extractors)
        : state(UNKNOWN)
        , extractorChain(extractors...)
    {}

    StoryChunkExtractionQueue& getExtractionQueue() { return chunkExtractionQueue; }

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

                    // each extractor in the chain would handle its own intermittent failure appropriately
                    extractorChain.process_chunk(story_chunk);

                    // free the memory or reset the chunk to the original state and return it to the pool of prealocated chunks
                    delete story_chunk;
                }
            }
            else
            {
                tl::thread::self().yield();
            }
        }
    }

    void startExtraction(int stream_count)
    {

        if(state == RUNNING)
        {
            LOG_INFO("[StoryChunkExtractionModule] ExtractionModule already running; ignoring start request.");
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
        LOG_DEBUG("[StoryChunkExtractionModule] Initiating shutdown: extractionQueue size {}",
                  chunkExtractionQueue.size());

        drainExtractionQueue();

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

    ExtractorChain<Args...> extractorChain;

    std::vector<tl::managed<tl::xstream>> extractionStreams;
    std::vector<tl::managed<tl::thread>> extractionThreads;
};


} // namespace chronolog

#endif
