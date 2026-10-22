#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cstdint>

#include <thallium.hpp>

#include <ChunkReceipt.h>
#include <StoryChunk.h>
#include "IngestionQueue.h"
#include "KeeperStoryPipeline.h"
#include "StoryChunkExtractionQueue.h"
#include "WalRecoveredState.h"


namespace chronolog
{

// The id a story recovered from the WAL is recorded and released under: the
// id computed from its names, the way the visor assigns it, so a WAL written by
// a build that computed ids differently still joins the live story; the id the
// WAL recorded when it has no names.
StoryId recoveredStoryId(RecoveredStory const&);


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
                    uint32_t watermark_resend_timeout_secs = 300)
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

    // A durable write for a story this keeper has no pipeline for (it
    // restarted while a client held the story, and the visor's start has not
    // reached it yet): starts one with the keeper's own durations and now as
    // its start time, and lets it decay as a stopped story's pipeline does
    // unless a start_story_recording keeps it. Unlike startStoryRecording it
    // leaves the retention store's drop mark for the id in place, so a stale
    // writer cannot undo a destroy. A pipeline that exists is left as it is.
    int ensureStoryPipeline(ChronicleName const&, StoryName const&, StoryId const&);

    int stopStoryRecording(StoryId const&);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    // Seal late/orphaned events for already-retired stories into recovery
    // StoryChunks handed to the extraction queue, so they are archived instead
    // of stranded in the ingestion orphan queue and dropped at shutdown.
    void sealOrphanedEvents();

    // At startup, before the keeper takes requests: hand the events WAL
    // recovery read back to the retention store as sealed chunks aligned to
    // story_chunk_duration_secs, under recoveredStoryId, and release their
    // stories' tails as sealOrphanedEvents does. A story without names is
    // logged and not shipped. Returns the number of chunks handed over.
    std::size_t replayRecoveredEvents(RecoveredState const&);

    // Grapher watermark report: everything below w for the story is durable;
    // forwards to the retention store's confirmPersisted.
    void applyWatermarkReport(StoryId const&, uint64_t w);

    // Grapher report for one story: its watermark and the receipts it has not
    // written yet (see ChunkReceipt.h); forwards to the retention store.
    void applyWatermarkReport(StoryId const&, StoryWatermarkReport const& report);

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    KeeperDataStore(KeeperDataStore const&) = delete;

    KeeperDataStore& operator=(KeeperDataStore const&) = delete;

    // Creates the story's pipeline and engages it with the ingestion queue.
    // Caller holds dataStoreMutex and has checked the story has none. Returns
    // the pipeline, or nullptr when it could not be added.
    KeeperStoryPipeline* addStoryPipeline(ChronicleName const&, StoryName const&, StoryId const&, uint64_t start_time);

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
