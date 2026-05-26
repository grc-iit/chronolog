#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <functional>

#include <thallium.hpp>

#include <StoryChunk.h>
#include "IngestionQueue.h"
#include "KeeperStoryPipeline.h"
#include "StoryChunkExtractionQueue.h"


namespace chronolog
{


class KeeperDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active stories
        SHUTTING_DOWN = 2 // Shutting down services
    };


public:
    // Callback type used by the synchronous flush path on ReleaseStory:
    // returns CL_SUCCESS once the chunk has been handed off to (and acked by)
    // the next stage of the recording pipeline (the Grapher, for the Keeper).
    using SyncChunkProcessor = std::function<int(StoryChunk*)>;

    KeeperDataStore(IngestionQueue& ingestion_queue,
                    StoryChunkExtractionQueue& extraction_queue,
                    uint32_t max_chunk_size = 4096,
                    uint32_t story_chunk_duration_secs = 30,
                    uint32_t acceptance_window_secs = 60,
                    uint32_t inactive_pipeline_delay_secs = 300,
                    SyncChunkProcessor sync_chunk_processor = nullptr)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
        , syncChunkProcessor(std::move(sync_chunk_processor))

    {}

    ~KeeperDataStore();

    bool is_running() const { return (RUNNING == state); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const&,
                            StoryName const&,
                            StoryId const&,
                            uint64_t start_time,
                            uint32_t time_chunk_ranularity = 30,
                            uint32_t access_window = 60);

    int stopStoryRecording(StoryId const&);

    // Synchronous-flush variant of stopStoryRecording. Disengages the pipeline
    // from the ingestion queue, drains both event deques into the pipeline,
    // finalizes the pipeline into a local vector of StoryChunks, and pushes
    // each chunk synchronously through the SyncChunkProcessor (the RDMA
    // extractor to the Grapher) before returning. Used by ReleaseStory so the
    // event-loss window on Release is closed end-to-end.
    int flushAndStopStoryRecording(StoryId const&);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    KeeperDataStore(KeeperDataStore const&) = delete;

    KeeperDataStore& operator=(KeeperDataStore const&) = delete;

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    IngestionQueue& theIngestionQueue;
    StoryChunkExtractionQueue& theExtractionQueue;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;
    SyncChunkProcessor syncChunkProcessor;

    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, KeeperStoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<KeeperStoryPipeline*, uint64_t>> pipelinesWaitingForExit;
};

} // namespace chronolog
#endif
