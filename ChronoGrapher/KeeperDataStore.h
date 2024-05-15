#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include <thallium.hpp>

#include "ChunkIngestionQueue.h"
#include "StoryPipeline.h"
#include "StoryChunkExtractionQueue.h"
#include "DataStoreConfiguration.h"

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
    KeeperDataStore(ChunkIngestionQueue &, StoryChunkExtractionQueue &, DataStoreConfiguration const &);

    ~KeeperDataStore();

    bool is_running() const
    { return (RUNNING == state); }

    bool is_shutting_down() const
    { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const &, StoryName const &, StoryId const &, uint64_t start_time
                            , uint32_t time_chunk_ranularity = 30, uint32_t access_window = 300);

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
    ChunkIngestionQueue &theIngestionQueue;
    StoryChunkExtractionQueue &theExtractionQueue;
    std::vector <thallium::managed <thallium::xstream>> dataStoreStreams;
    std::vector <thallium::managed <thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map <StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map <StoryId, std::pair <StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

    // data store internal defaults  that can be overwriten through DataStoreConfiguration values
    uint32_t max_story_chunk_size = 65536;  // 2^16 = 65536 
    uint32_t story_chunk_time_granularity = 60;// seconds
    uint32_t story_chunk_access_window = 120;  // seconds
    uint32_t story_pipeline_exit_window = 300; // seconds

    std::string csv_file_extraction_dir = "/tmp/";

};

}
#endif
