#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <functional>
#include <thread>

#include <thallium.hpp>

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
                    uint32_t max_chunk_size = 4096,
                    uint32_t story_chunk_duration_secs = 30,
                    uint32_t acceptance_window_secs = 60,
                    uint32_t inactive_pipeline_delay_secs = 300,
                    uint32_t data_collection_poll_interval_us = 1000000,
                    std::function<int(StoryId const&)> story_drain_complete_callback = {})
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
        , data_collection_poll_interval_us(data_collection_poll_interval_us)
        , storyDrainCompleteCallback(std::move(story_drain_complete_callback))

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

    int flushStoryRecording(StoryId const&, uint64_t extraction_drain_timeout_ms, bool async_drain_complete = false);

    int collectTailEvents(StoryId const&, chrono_time, chrono_time, std::vector<LogEvent>&);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count, int threads_per_stream = 2);

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
    uint32_t data_collection_poll_interval_us;
    std::function<int(StoryId const&)> storyDrainCompleteCallback;

    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, KeeperStoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<KeeperStoryPipeline*, uint64_t>> pipelinesWaitingForExit;
};

} // namespace chronolog
#endif
