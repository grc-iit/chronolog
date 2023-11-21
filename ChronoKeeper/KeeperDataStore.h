#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include <thallium.hpp>

#include "IngestionQueue.h"
#include "StoryPipeline.h"
#include "StoryChunkExtractionQueue.h"


namespace chronolog
{


class KeeperDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0, RUNNING = 1,    //  active stories
        SHUTTING_DOWN = 2    // Shutting down services
    };


public:
    KeeperDataStore(IngestionQueue &ingestion_queue, StoryChunkExtractionQueue &extraction_queue): state(UNKNOWN)
                                                                                                   , theIngestionQueue(
                    ingestion_queue), theExtractionQueue(extraction_queue)
    {}

    ~KeeperDataStore();

    bool is_running() const
    { return (RUNNING == state); }

    bool is_shutting_down() const
    { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const &, StoryName const &, StoryId const &, uint64_t start_time
                            , uint32_t time_chunk_ranularity = 30, uint32_t access_window = 60);

    int stopStoryRecording(StoryId const &);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

private:
    KeeperDataStore(KeeperDataStore const &) = delete;

    KeeperDataStore &operator=(KeeperDataStore const &) = delete;

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    IngestionQueue &theIngestionQueue;
    StoryChunkExtractionQueue &theExtractionQueue;
    std::vector <thallium::managed <thallium::xstream>> dataStoreStreams;
    std::vector <thallium::managed <thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map <StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map <StoryId, std::pair <StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

};

}
#endif
