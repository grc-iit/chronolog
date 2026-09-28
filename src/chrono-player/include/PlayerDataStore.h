#ifndef PLAYER_DATA_STORE_H
#define PLAYER_DATA_STORE_H

#include <string>
#include <cstdint>
#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <thallium.hpp>

#include <StoryChunkIngestionQueue.h>
#include <StoryPipeline.h>
#include <ServiceId.h>


namespace chronolog
{


class PlayerDataStore
{

    enum DataStoreState
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active stories
        SHUTTING_DOWN = 2 // Shutting down services
    };


public:
    PlayerDataStore(StoryChunkIngestionQueue& ingestion_queue,
                    uint32_t max_chunk_size = 4096,
                    uint32_t story_chunk_duration_secs = 60,
                    uint32_t acceptance_window_secs = 300,
                    uint32_t inactive_pipeline_delay_secs = 300)
        : state(UNKNOWN)
        , theIngestionQueue(ingestion_queue)
        , story_chunk_size(max_chunk_size)
        , story_chunk_duration_secs(story_chunk_duration_secs)
        , acceptance_window_secs(acceptance_window_secs)
        , inactive_pipeline_delay_secs(inactive_pipeline_delay_secs)
    {}

    ~PlayerDataStore();

    bool is_running() const { return (RUNNING == state); }

    bool is_shutting_down() const { return (SHUTTING_DOWN == state); }

    int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&, uint64_t start_time);

    int stopStoryRecording(StoryId const&);

    void collectIngestedEvents();

    void extractDecayedStoryChunks();

    void retireDecayedPipelines();

    void startDataCollection(int stream_count);

    void shutdownDataCollection();

    void dataCollectionTask();

    uint64_t get_active_window_boundary() const;

    int get_active_story_events(StoryId const& story_id,
                                uint64_t start_time,
                                uint64_t end_time,
                                std::vector<Event>& events);

    // Keeper roster for the story (recording-service identities), delivered
    // by the visor with every story start and replaced wholesale each time.
    // The playback service fans replay hot fetches out over it. Mid-story
    // keeper membership changes do not refresh the roster until the next
    // story start — documented limitation; a missing keeper only shrinks the
    // hot set (the archive covers everything below the remaining keepers'
    // floors).
    void setStoryKeepers(StoryId const& story_id, std::vector<ServiceId> const& keeper_services)
    {
        std::lock_guard<std::mutex> lock(storyKeeperMutex);
        storyKeepers[story_id] = keeper_services;
    }

    std::vector<ServiceId> getStoryKeepers(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(storyKeeperMutex);
        auto roster_iter = storyKeepers.find(story_id);
        return (roster_iter == storyKeepers.end()) ? std::vector<ServiceId>{} : roster_iter->second;
    }

private:
    PlayerDataStore(PlayerDataStore const&) = delete;

    PlayerDataStore& operator=(PlayerDataStore const&) = delete;

    DataStoreState state;
    std::mutex dataStoreStateMutex;
    StoryChunkIngestionQueue& theIngestionQueue;
    std::vector<thallium::managed<thallium::xstream>> dataStoreStreams;
    std::vector<thallium::managed<thallium::thread>> dataStoreThreads;

    std::mutex dataStoreMutex;
    std::unordered_map<StoryId, StoryPipeline*> theMapOfStoryPipelines;
    std::unordered_map<StoryId, std::pair<StoryPipeline*, uint64_t>> pipelinesWaitingForExit;

    uint32_t story_chunk_size;
    uint32_t story_chunk_duration_secs;
    uint32_t acceptance_window_secs;
    uint32_t inactive_pipeline_delay_secs;

    mutable std::mutex storyKeeperMutex;
    std::unordered_map<StoryId, std::vector<ServiceId>> storyKeepers;
};

} // namespace chronolog
#endif
