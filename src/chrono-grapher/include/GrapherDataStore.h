#ifndef GRAPHER_DATA_STORE_H
#define GRAPHER_DATA_STORE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <thallium.hpp>

#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>

#include "ChunkIngestionQueue.h"


namespace chronolog
{

class ChronoGrapherExtractionChain;


class GrapherDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active stories
        SHUTTING_DOWN = 2 // Shutting down services
    };


public:
    GrapherDataStore(ChunkIngestionQueue& ingestion_queue,
                     StoryChunkExtractionQueue& extraction_queue,
                     uint32_t max_chunk_size = 4096,
                     uint32_t story_chunk_duration_secs = 60,
                     uint32_t acceptance_window_secs = 180,
                     uint32_t inactive_pipeline_delay_secs = 300,
                     ChronoGrapherExtractionChain* extraction_chain = nullptr)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
        , theExtractionChain(extraction_chain)
        , destroyWorkerShouldExit(false)
    {}

    ~GrapherDataStore();

    bool is_running() const { return (RUNNING == state); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&, uint64_t start_time);

    int stopStoryRecording(StoryId const&);

    // Async destroy of a single story. Atomically unhooks the live pipeline,
    // drains in-flight chunks, finalizes the timeline, and enqueues a destroy
    // task for the dedicated worker thread which (a) stashes the remaining
    // chunks for the extraction chain to persist, (b) waits until extraction
    // has drained them, and (c) deletes the matching HDF5 files via the
    // injected ChronoGrapherExtractionChain reference. The RPC returns as
    // soon as the task is enqueued; ordering between persistence and deletion
    // is enforced inside the Grapher.
    int destroyStory(StoryId const&, ChronicleName const& chronicle, StoryName const& story);

    // Async destroy of every story in a chronicle. Same ordering contract as
    // destroyStory: the worker waits for extraction to drain all enqueued
    // chunks before deleting any HDF5 files.
    int destroyChronicle(ChronicleName const& chronicle);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    GrapherDataStore(GrapherDataStore const&) = delete;

    GrapherDataStore& operator=(GrapherDataStore const&) = delete;

    // Work submitted to the destroy worker. Carries either a single story or
    // a whole chronicle's worth of pipelines that have already been unhooked
    // from the live maps and drained into remainingChunks.
    struct DestroyTask
    {
        enum class Kind
        {
            Story,
            Chronicle
        };

        Kind kind;
        ChronicleName chronicleName;
        StoryName storyName; // only meaningful for Kind::Story
        std::vector<StoryPipeline*> pipelines;
        std::vector<StoryChunk*> remainingChunks;
    };

    void destroyWorkerTask();
    void enqueueDestroyTask(DestroyTask&& task);
    void drainExtractionQueueOrTimeout();

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    ChunkIngestionQueue& theIngestionQueue;
    StoryChunkExtractionQueue& theExtractionQueue;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;
    ChronoGrapherExtractionChain* theExtractionChain;

    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

    // Destroy worker: a dedicated std::thread (not an Argobots xstream) so
    // it can block on the extraction-queue drain without starving the data
    // collection ULTs. Tasks are enqueued by destroyStory / destroyChronicle
    // under destroyMutex; the worker pops them one at a time.
    std::thread destroyWorkerThread;
    std::mutex destroyMutex;
    std::condition_variable destroyCv;
    std::deque<DestroyTask> destroyQueue;
    std::atomic<bool> destroyWorkerShouldExit;
};

} // namespace chronolog
#endif
