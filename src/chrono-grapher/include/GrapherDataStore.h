#ifndef GRAPHER_DATA_STORE_H
#define GRAPHER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>

#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>

#include "ChunkIngestionQueue.h"


namespace chronolog
{


class GrapherDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active stories
        SHUTTING_DOWN = 2 // Shutting down services
    };


public:
    // File-deletion hooks used by destroy_story / destroy_chronicle to remove
    // persisted HDF5 files from the Grapher's archive directory.
    using DeleteStoryFilesFn = std::function<int(ChronicleName const&, StoryName const&)>;
    using DeleteChronicleFilesFn = std::function<int(ChronicleName const&)>;

    GrapherDataStore(ChunkIngestionQueue& ingestion_queue,
                     StoryChunkExtractionQueue& extraction_queue,
                     uint32_t max_chunk_size = 4096,
                     uint32_t story_chunk_duration_secs = 60,
                     uint32_t acceptance_window_secs = 180,
                     uint32_t inactive_pipeline_delay_secs = 300,
                     DeleteStoryFilesFn delete_story_files = nullptr,
                     DeleteChronicleFilesFn delete_chronicle_files = nullptr)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , theExtractionQueue(extraction_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
        , deleteStoryFiles(std::move(delete_story_files))
        , deleteChronicleFiles(std::move(delete_chronicle_files))
    {}

    ~GrapherDataStore();

    bool is_running() const { return (RUNNING == state); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&, uint64_t start_time);

    int stopStoryRecording(StoryId const&);

    // Destroy persisted state for a single story: cancel any in-memory pipeline
    // (no flush — destroy discards) and delete the matching HDF5 files. Safe to
    // call on Graphers that never recorded this story; deletion hooks filter by
    // filename and emit zero deletes.
    int destroyStory(StoryId const&, ChronicleName const& chronicle, StoryName const& story);

    // Destroy persisted state for every story in a chronicle. Cancels any
    // in-memory pipelines belonging to the chronicle and deletes all HDF5 files
    // matching <chronicle>.*.vlen.h5 in the archive directory.
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

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    ChunkIngestionQueue& theIngestionQueue;
    StoryChunkExtractionQueue& theExtractionQueue;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;
    DeleteStoryFilesFn deleteStoryFiles;
    DeleteChronicleFilesFn deleteChronicleFiles;

    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<StoryPipeline*, uint64_t>> pipelinesWaitingForExit;
};

} // namespace chronolog
#endif
