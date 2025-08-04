#ifndef GRAPHER_DATA_STORE_H
#define GRAPHER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include <thallium.hpp>

#include "ChunkIngestionQueue.h"
#include "StoryPipeline.h"
#include "StoryChunkExtractionQueue.h"


namespace chronolog
{


class GrapherDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0, RUNNING = 1,    //  active stories
        SHUTTING_DOWN = 2    // Shutting down services
    };


public:
    GrapherDataStore(ChunkIngestionQueue &ingestion_queue, StoryChunkExtractionQueue &extraction_queue
                    , uint32_t max_chunk_size = 4096, uint32_t story_chunk_duration_secs = 60
                    , uint32_t acceptance_window_secs = 180, uint32_t inactive_pipeline_delay_secs = 300 )
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
    {}

    ~GrapherDataStore();

    bool is_running() const
    { return (RUNNING == state); }

    bool is_shutting_down() const
    { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const &, StoryName const &, StoryId const &, uint64_t start_time);

    int stopStoryRecording(StoryId const &);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    GrapherDataStore(GrapherDataStore const &) = delete;

    GrapherDataStore &operator=(GrapherDataStore const &) = delete;

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    ChunkIngestionQueue &theIngestionQueue;
    StoryChunkExtractionQueue &theExtractionQueue;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;
    
    std::vector <thallium::managed <thallium::xstream>> dataStoreStreams;
    std::vector <thallium::managed <thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map <StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map <StoryId, std::pair <StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

};

}
#endif
