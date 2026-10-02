// When the visor stops a story's recording, the player keeps the story's
// pipeline for inactive_story_delay_secs (300 in the deploy template) before it
// retires it. Until then the pipeline still answers queries for the story.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <PlayerDataStore.h>

namespace chl = chronolog;

namespace
{
constexpr chl::StoryId kStory = 5;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "player_pipeline_retirement_test_logger");
        done = true;
    }
}

uint64_t nowNs() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }

int queryStory(chl::PlayerDataStore& data_store)
{
    std::vector<chl::Event> events;
    return data_store.get_active_story_events(kStory, 0, UINT64_MAX, events);
}
} // namespace

TEST(PlayerPipelineRetirement, StoppedStoryKeepsItsPipelineForTheWholeDelay)
{
    ensureLogger();
    thallium::abt argobots; // the data store logs Argobots thread ids
    chl::StoryChunkIngestionQueue ingestion_queue;
    chl::PlayerDataStore data_store(ingestion_queue, 4096, 60, 300, /*inactive delay=*/5);
    ASSERT_EQ(data_store.startStoryRecording("C", "S", kStory, nowNs()), chl::CL_SUCCESS);
    data_store.stopStoryRecording(kStory);

    // 5 s is 5,000,000,000 ns, which wraps to about 0.7 s in 32 bits
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    data_store.retireDecayedPipelines();
    EXPECT_EQ(queryStory(data_store), chl::CL_SUCCESS);
}

TEST(PlayerPipelineRetirement, StoppedStoryRetiresOnceTheDelayHasPassed)
{
    ensureLogger();
    thallium::abt argobots;
    chl::StoryChunkIngestionQueue ingestion_queue;
    chl::PlayerDataStore data_store(ingestion_queue, 4096, 60, 300, /*inactive delay=*/1);
    ASSERT_EQ(data_store.startStoryRecording("C", "S", kStory, nowNs()), chl::CL_SUCCESS);
    data_store.stopStoryRecording(kStory);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    data_store.retireDecayedPipelines();
    EXPECT_EQ(queryStory(data_store), chl::CL_ERR_NOT_ACQUIRED);
}
