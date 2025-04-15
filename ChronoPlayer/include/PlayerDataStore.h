#ifndef PLAYER_DATA_STORE_H
#define PLAYER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include <thallium.hpp>

#include "StoryChunkIngestionQueue.h"
#include "StoryPipeline.h"
#include "StoryChunkExtractionQueue.h"


namespace chronolog
{


class PlayerDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0, 
        RUNNING = 1,    //  active stories
        SHUTTING_DOWN = 2    // Shutting down services
    };


public:
    PlayerDataStore(StoryChunkIngestionQueue &ingestion_queue, StoryChunkExtractionQueue &extraction_queue)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
    {}

    ~PlayerDataStore();

    bool is_running() const
    { return (RUNNING == state); }

    bool is_shutting_down() const
    { return (SHUTTING_DOWN == state); }
/*
    int startStoryRecording(ChronicleName const &, StoryName const &, StoryId const &, uint64_t start_time
                            , uint32_t time_chunk_ranularity = 30, uint32_t access_window = 300);

    int stopStoryRecording(StoryId const &);
*/
    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    PlayerDataStore(PlayerDataStore const &) = delete;

    PlayerDataStore &operator=(PlayerDataStore const &) = delete;

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    StoryChunkIngestionQueue &theIngestionQueue;
    StoryChunkExtractionQueue &theExtractionQueue;
    std::vector <thallium::managed <thallium::xstream>> dataStoreStreams;
    std::vector <thallium::managed <thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map <StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map <StoryId, std::pair <StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

};

}
#endif
