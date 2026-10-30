// A restarted keeper turns the events WAL recovery read back into sealed
// chunks before it registers: one chunk per story_chunk_duration_secs slot,
// under the story id recomputed from the story's names, retained by the
// retention store like any sealed chunk until the grapher confirms it.
//
// The keeper data path runs in-process here (no daemons, no network), as in
// chrono_keeper_story_retirement_test.cpp.

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_types.h>

#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryIdOf.h>
#include <WalObserver.h>
#include <WalRecoveredState.h>

namespace chl = chronolog;

namespace
{
void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "wal_replay_test_logger");
        done = true;
    }
}

constexpr uint32_t kChunkDurationSecs = 30;
constexpr uint64_t kChunkDurationNs = uint64_t{kChunkDurationSecs} * 1000000000ULL;
constexpr std::size_t kTailCapacity = 1024;

// A fixed time well past the epoch, on a chunk boundary; replay does not look
// at the clock, so the tests need not either.
constexpr uint64_t kBase = 1000 * kChunkDurationNs;

// The keeper's data store and the queues it works on, with no services.
struct Keeper
{
    chl::IngestionQueue ingestion_queue;
    chl::StoryChunkExtractionQueue extraction_queue;
    chl::KeeperChunkRetentionStore retention_store{extraction_queue, kTailCapacity};
    chl::KeeperDataStore data_store{ingestion_queue,
                                    extraction_queue,
                                    retention_store,
                                    /*max_chunk_size=*/4096,
                                    /*story_chunk_duration_secs=*/kChunkDurationSecs,
                                    /*acceptance_window_secs=*/0,
                                    /*inactive_pipeline_delay_secs=*/300};
};

// What a chunk looked like when it left the extraction queue.
struct ShippedChunk
{
    chl::StoryId story_id;
    uint64_t start;
    uint64_t end;
    std::vector<chl::LogEvent> events;
};

// Take every queued chunk off the extraction queue, note what it holds, and
// hand it back to the store as acknowledged by the grapher.
std::vector<ShippedChunk> shipQueued(Keeper& keeper)
{
    std::vector<ShippedChunk> shipped;
    while(chl::StoryChunk* chunk = keeper.extraction_queue.ejectStoryChunk())
    {
        ShippedChunk seen{chunk->getStoryId(), chunk->getStartTime(), chunk->getEndTime(), {}};
        for(auto it = chunk->begin(); it != chunk->end(); ++it) { seen.events.push_back(it->second); }
        shipped.push_back(std::move(seen));
        keeper.retention_store.markShipped(chunk);
    }
    return shipped;
}

// A recovered story as WalRecovery leaves it: events keyed by identity, each
// carrying the story id the WAL recorded.
chl::RecoveredStory recoveredStory(chl::StoryId wal_story_id,
                                   bool has_names,
                                   std::vector<uint64_t> const& times,
                                   chl::ChronicleName const& chronicle = "chron",
                                   chl::StoryName const& story = "story")
{
    chl::RecoveredStory recovered;
    recovered.wal_story_id = wal_story_id;
    recovered.has_names = has_names;
    if(has_names)
    {
        recovered.chronicle = chronicle;
        recovered.story = story;
    }
    uint32_t index = 0;
    for(uint64_t time: times)
    {
        chl::LogEvent event(wal_story_id, time, /*client=*/7, index++, "event-" + std::to_string(time));
        chl::EventSequence const seq{event.time(), event.getClientId(), event.index()};
        recovered.events.emplace(seq, event);
        recovered.copies[seq].push_back({/*segment=*/1, /*frame bytes=*/64});
    }
    return recovered;
}

class RecordingObserver: public chl::WalObserver
{
public:
    void chunkWritten(chl::StoryId story_id, chl::StoryChunk const& chunk) override
    {
        for(auto it = chunk.begin(); it != chunk.end(); ++it) { written.insert({story_id, it->first}); }
    }
    void chunkDiscarded(chl::StoryId, chl::StoryChunk const&) override {}
    void eventDiscarded(chl::StoryId, chl::EventSequence const&) override {}
    void storyDropped(chl::StoryId) override {}

    std::set<std::pair<chl::StoryId, chl::EventSequence>> written;
};

// A watermark an hour past every chunk these tests seal (not
// kStoryDroppedWatermark, which frees chunks whatever their state).
uint64_t coveringWatermark() { return kBase + 3600ULL * 1000000000ULL; }
} // namespace

TEST(KeeperWalReplay, ChunksAlignToChunkDuration)
{
    ensureLogger();
    Keeper keeper;

    // Slots 0, 1 and 3 hold events, with some on the slot's first and last
    // nanosecond; slot 2 is empty and must not produce a chunk.
    std::vector<uint64_t> const times = {kBase,
                                         kBase + 5,
                                         kBase + kChunkDurationNs - 1,
                                         kBase + kChunkDurationNs,
                                         kBase + kChunkDurationNs + 1000,
                                         kBase + 3 * kChunkDurationNs + 42};
    chl::RecoveredStory const story = recoveredStory(chl::storyIdOf("chron", "story"), true, times);
    chl::RecoveredState state;
    state.segments = {1};
    state.stories.emplace(story.wal_story_id, story);

    EXPECT_EQ(keeper.data_store.replayRecoveredEvents(state), 3u);

    std::vector<ShippedChunk> const shipped = shipQueued(keeper);
    ASSERT_EQ(shipped.size(), 3u);
    std::set<uint64_t> starts;
    std::size_t event_count = 0;
    for(ShippedChunk const& chunk: shipped)
    {
        EXPECT_EQ(chunk.start % kChunkDurationNs, 0u);
        EXPECT_EQ(chunk.end - chunk.start, kChunkDurationNs);
        for(chl::LogEvent const& event: chunk.events)
        {
            EXPECT_GE(event.time(), chunk.start);
            EXPECT_LT(event.time(), chunk.end);
        }
        starts.insert(chunk.start);
        event_count += chunk.events.size();
    }
    EXPECT_EQ(starts, (std::set<uint64_t>{kBase, kBase + kChunkDurationNs, kBase + 3 * kChunkDurationNs}));
    EXPECT_EQ(event_count, times.size());
}

TEST(KeeperWalReplay, IdsComeFromNames)
{
    ensureLogger();
    Keeper keeper;

    chl::StoryId const hashed = chl::storyIdOf("chron", "story");
    chl::StoryId const wal_story_id = hashed + 1; // written by a build that computed ids differently
    chl::RecoveredStory const story = recoveredStory(wal_story_id, true, {kBase + 1, kBase + 2});
    EXPECT_EQ(chl::recoveredStoryId(story), hashed);

    chl::RecoveredState state;
    state.stories.emplace(wal_story_id, story);
    EXPECT_EQ(keeper.data_store.replayRecoveredEvents(state), 1u);

    EXPECT_EQ(keeper.retention_store.retainedChunkCount(hashed), 1u);
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(wal_story_id), 0u);

    std::vector<ShippedChunk> const shipped = shipQueued(keeper);
    ASSERT_EQ(shipped.size(), 1u);
    EXPECT_EQ(shipped[0].story_id, hashed);
    ASSERT_EQ(shipped[0].events.size(), 2u);
    for(chl::LogEvent const& event: shipped[0].events) { EXPECT_EQ(event.getStoryId(), hashed); }
}

TEST(KeeperWalReplay, NamelessStoryIsNotShipped)
{
    ensureLogger();
    Keeper keeper;

    chl::StoryId const wal_story_id = 4242;
    chl::RecoveredStory const story = recoveredStory(wal_story_id, false, {kBase + 1, kBase + 2});
    // the WAL keeps tracking it under the only id it has
    EXPECT_EQ(chl::recoveredStoryId(story), wal_story_id);

    chl::RecoveredState state;
    state.stories.emplace(wal_story_id, story);
    EXPECT_EQ(keeper.data_store.replayRecoveredEvents(state), 0u);

    EXPECT_TRUE(shipQueued(keeper).empty());
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(wal_story_id), 0u);
}

TEST(KeeperWalReplay, ReplayedChunksAreRetainedUntilConfirmed)
{
    ensureLogger();
    Keeper keeper;
    RecordingObserver observer;
    keeper.retention_store.setWalObserver(&observer);

    chl::StoryId const hashed = chl::storyIdOf("chron", "story");
    chl::RecoveredStory const story =
            recoveredStory(hashed + 1, true, {kBase + 1, kBase + kChunkDurationNs + 1, kBase + kChunkDurationNs + 2});
    chl::RecoveredState state;
    state.stories.emplace(story.wal_story_id, story);
    ASSERT_EQ(keeper.data_store.replayRecoveredEvents(state), 2u);

    // queued for the grapher, not yet shipped
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(hashed), 2u);
    ASSERT_EQ(shipQueued(keeper).size(), 2u);
    // shipped, but no watermark covers them yet: they stay, and the WAL keeps
    // their records
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(hashed), 2u);
    EXPECT_TRUE(observer.written.empty());

    // the grapher's report comes back under the recomputed id; the tail was
    // released at replay, so the chunks free at once
    keeper.data_store.applyWatermarkReport(hashed, coveringWatermark());
    EXPECT_EQ(keeper.retention_store.retainedChunkCount(hashed), 0u);

    std::set<std::pair<chl::StoryId, chl::EventSequence>> expected;
    for(auto const& entry: story.events) { expected.insert({hashed, entry.first}); }
    EXPECT_EQ(observer.written, expected);
}
