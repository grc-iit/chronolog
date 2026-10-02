// When the visor stops a story's recording, the grapher keeps the story's
// pipeline for inactive_story_delay_secs (300 in the deploy template) before it
// writes out every remaining window and retires the pipeline, so that chunks
// the keepers seal after the release still join an open pipeline. Retiring
// writes every window, empty ones included, so the extraction queue shows
// whether it has happened.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ChunkIngestionQueue.h>
#include <GrapherDataStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

namespace
{
constexpr chl::StoryId kStory = 5;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "grapher_pipeline_retirement_test_logger");
        done = true;
    }
}

uint64_t nowNs() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }
} // namespace

TEST(GrapherPipelineRetirement, StoppedStoryKeepsItsPipelineForTheWholeDelay)
{
    ensureLogger();
    thallium::abt argobots; // the data store logs Argobots thread ids
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::ChunkIngestionQueue ingestion_queue;
    chl::GrapherDataStore data_store(ingestion_queue, extraction_queue, 4096, 60, 180, /*inactive delay=*/5);
    ASSERT_EQ(data_store.startStoryRecording("C", "S", kStory, nowNs()), chl::CL_SUCCESS);
    data_store.stopStoryRecording(kStory);

    // 5 s is 5,000,000,000 ns, which wraps to about 0.7 s in 32 bits
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    data_store.retireDecayedPipelines();
    EXPECT_EQ(extraction_queue.size(), 0);
}

TEST(GrapherPipelineRetirement, StoppedStoryRetiresOnceTheDelayHasPassed)
{
    ensureLogger();
    thallium::abt argobots;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::ChunkIngestionQueue ingestion_queue;
    chl::GrapherDataStore data_store(ingestion_queue, extraction_queue, 4096, 60, 180, /*inactive delay=*/1);
    ASSERT_EQ(data_store.startStoryRecording("C", "S", kStory, nowNs()), chl::CL_SUCCESS);
    data_store.stopStoryRecording(kStory);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    data_store.retireDecayedPipelines();
    EXPECT_GT(extraction_queue.size(), 0);
}
