// A keeper frees a sealed chunk only once it is shipped, the grapher's
// persisted watermark covers it, and none of its events is still in the
// story's tail index. Only tail_capacity eviction takes events out of that
// index, so a story that retires before it has logged tail_capacity events
// would keep its chunks until the keeper restarts. Retiring a story has to
// release its tail, including chunks sealed for it after retirement.
//
// The keeper data path runs in-process here (no daemons, no network), as in
// chrono_keeper_release_dataloss_test.cpp.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include <abt.h>

#include <chrono_monitor.h>
#include <chronolog_types.h>

#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

namespace
{
void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "story_retirement_test_logger");
        done = true;
    }
}

// KeeperDataStore's maintenance methods log tl::thread::self_id(), which needs
// an initialized Argobots runtime.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

uint64_t nowNs() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }

// Deliver every queued chunk back to the store as acknowledged by the grapher,
// as the keeper's extraction drain does. Returns the number of chunks.
int shipQueued(chl::StoryChunkExtractionQueue& extraction_queue, chl::KeeperChunkRetentionStore& retention_store)
{
    int shipped = 0;
    while(chl::StoryChunk* chunk = extraction_queue.ejectStoryChunk())
    {
        retention_store.markShipped(chunk);
        ++shipped;
    }
    return shipped;
}

// far above the handful of events each test logs, so eviction never releases them
constexpr std::size_t kTailCapacity = 1024;
} // namespace

TEST(KeeperStoryRetirement, RetiredStoryFreesItsChunksOnceDurable)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestion_queue;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::KeeperChunkRetentionStore retention_store(extraction_queue, kTailCapacity);
    chl::KeeperDataStore data_store(ingestion_queue,
                                    extraction_queue,
                                    retention_store,
                                    /*max_chunk_size=*/4096,
                                    /*story_chunk_duration_secs=*/30,
                                    /*acceptance_window_secs=*/0,
                                    /*inactive_pipeline_delay_secs=*/300);

    chl::StoryId const story_id = 77;
    uint64_t const base = nowNs();
    data_store.startStoryRecording("chron", "story", story_id, base, /*granularity=*/30, /*window=*/0);
    for(uint32_t i = 0; i < 3; ++i)
    {
        ingestion_queue.ingestLogEvent(chl::LogEvent(story_id, base + 1 + i, /*client=*/1, i, "event"));
    }
    data_store.stopStoryRecording(story_id);
    // acceptance window 0: the pipeline retires now and seals its chunk
    data_store.retireDecayedPipelines();

    ASSERT_GT(shipQueued(extraction_queue, retention_store), 0);
    // shipped, but no watermark covers it yet: it must stay
    EXPECT_GT(retention_store.retainedChunkCount(story_id), 0u);

    data_store.applyWatermarkReport(story_id, UINT64_MAX);
    EXPECT_EQ(retention_store.retainedChunkCount(story_id), 0u);
}

TEST(KeeperStoryRetirement, ChunkSealedFromLateEventsAfterRetirementIsFreedOnceDurable)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestion_queue;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::KeeperChunkRetentionStore retention_store(extraction_queue, kTailCapacity);
    chl::KeeperDataStore data_store(ingestion_queue,
                                    extraction_queue,
                                    retention_store,
                                    /*max_chunk_size=*/4096,
                                    /*story_chunk_duration_secs=*/30,
                                    /*acceptance_window_secs=*/0,
                                    /*inactive_pipeline_delay_secs=*/300);

    chl::StoryId const story_id = 78;
    uint64_t const base = nowNs();
    data_store.startStoryRecording("chron", "story", story_id, base, /*granularity=*/30, /*window=*/0);
    ingestion_queue.ingestLogEvent(chl::LogEvent(story_id, base + 1, /*client=*/1, 0, "on-time"));
    data_store.stopStoryRecording(story_id);
    data_store.retireDecayedPipelines();
    int const shipped_at_retirement = shipQueued(extraction_queue, retention_store);
    ASSERT_GT(shipped_at_retirement, 0);

    // reaches the keeper after its story retired: parked as an orphan, then
    // sealed into a recovery chunk by the next maintenance pass
    ingestion_queue.ingestLogEvent(chl::LogEvent(story_id, base + 2, /*client=*/1, 1, "late"));
    data_store.retireDecayedPipelines();
    ASSERT_EQ(shipQueued(extraction_queue, retention_store), 1);

    data_store.applyWatermarkReport(story_id, UINT64_MAX);
    EXPECT_EQ(retention_store.retainedChunkCount(story_id), 0u);
}
