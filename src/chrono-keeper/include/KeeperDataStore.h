#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cstdint>

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
    KeeperDataStore(IngestionQueue& ingestion_queue,
                    StoryChunkExtractionQueue& extraction_queue,
                    KeeperChunkRetentionStore& tail_store,
                    uint32_t max_chunk_size = 4096,
                    uint32_t story_chunk_duration_secs = 30,
                    uint32_t acceptance_window_secs = 60,
                    uint32_t inactive_pipeline_delay_secs = 300,
                    uint32_t watermark_resend_timeout_secs = 720)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , theTailStore(tail_store)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
        , watermark_resend_timeout_secs(watermark_resend_timeout_secs)
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

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    // Seal late/orphaned events for already-retired stories into recovery
    // StoryChunks handed to the extraction queue, so they are archived instead
    // of stranded in the ingestion orphan queue and dropped at shutdown.
    void sealOrphanedEvents();

    // Grapher watermark report: everything below w for the story is durable;
    // forwards to the retention store's confirmPersisted.
    void applyWatermarkReport(StoryId const&, uint64_t w);

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
    KeeperChunkRetentionStore& theTailStore;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;
    uint32_t watermark_resend_timeout_secs;

    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, KeeperStoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<KeeperStoryPipeline*, uint64_t>> pipelinesWaitingForExit;
    // Names of stories that have been retired, retained so that late/orphaned
    // events (which carry only a storyId) can still be sealed into a correctly
    // named recovery chunk. Grows with the number of distinct retired stories;
    // a bounded cache is a possible future refinement.
    std::unordered_map<StoryId, std::pair<ChronicleName, StoryName>> retiredStoryNames;
};

} // namespace chronolog
#endif
