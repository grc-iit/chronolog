// Unit tests for the KeeperChunkRetentionStore retention cap
// (retention_cap_mb). The cap is an alarm, not a limit: when the grapher's
// persisted watermark W lags (an outage), retained chunks pile up past it, and
// the store must WARN once per crossing while keeping every chunk -- a sealed
// chunk that W does not cover yet may exist nowhere else.
//
// Separate from the main retention store test binary: the chrono logger is
// process-wide, its level is fixed by the first initialize(), and these tests
// need warnings on while the main test runs errors-only.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <chrono_monitor.h>
#include <StoryChunk.h>

#include <KeeperChunkRetentionStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

namespace
{
constexpr chl::StoryId kStory = 7;
constexpr std::size_t kCapMb = 1;
// 4 events x 100 KB: ~400 KB per chunk as the store counts it, so the third
// retained chunk crosses a 1 MB cap
constexpr int kEventsPerChunk = 4;
constexpr std::size_t kPayloadBytes = 100 * 1000;
constexpr char kCapWarning[] = "exceeds retention_cap_mb";

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::warn, "retention_cap_test_logger");
        done = true;
    }
}

// chunk i spans [i*100, (i+1)*100)
chl::StoryChunk* makeLargeChunk(int i)
{
    uint64_t const start = 100 * (uint64_t)i;
    auto* chunk = new chl::StoryChunk("chron", "story", kStory, start, start + 100, 64);
    for(int e = 0; e < kEventsPerChunk; e++)
    {
        chunk->insertEvent(
                chl::LogEvent(kStory, start + (uint64_t)e, 1, (chl::chrono_index)e, std::string(kPayloadBytes, 'x')));
    }
    return chunk;
}

// Drain every stashed chunk as acked by the grapher: shipped, W not reported.
void shipAll(chl::StoryChunkExtractionQueue& q, chl::KeeperChunkRetentionStore& store)
{
    while(chl::StoryChunk* chunk = q.ejectStoryChunk()) { store.markShipped(chunk); }
}

std::size_t countOccurrences(std::string const& text, std::string const& needle)
{
    std::size_t count = 0;
    for(auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
    {
        count++;
    }
    return count;
}
} // namespace

TEST(KeeperChunkRetentionCap, ExceededCapRetainsEveryChunk)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    // tail capacity 0: the tail releases at ingest, so only W holds the chunks
    chl::KeeperChunkRetentionStore store(q, 0, kCapMb);
    for(int i = 0; i < 4; i++) { store.ingestSealedChunk(kStory, makeLargeChunk(i)); }

    // the grapher acked every chunk, but its W never arrived
    shipAll(q, store);
    EXPECT_EQ(store.retainedChunkCount(kStory), 4u);

    uint64_t hot_floor = UINT64_MAX;
    bool truncated = true;
    auto events = store.fetchRange(kStory, 0, UINT64_MAX, 1000, hot_floor, truncated);
    EXPECT_EQ(events.size(), 4u * kEventsPerChunk);
    EXPECT_EQ(hot_floor, 0u);
    EXPECT_FALSE(truncated);
}

TEST(KeeperChunkRetentionCap, WarnsOncePerCrossing)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue q;
    chl::KeeperChunkRetentionStore store(q, 0, kCapMb);

    // crosses at the third chunk, stays above at the fourth
    testing::internal::CaptureStdout();
    for(int i = 0; i < 4; i++) { store.ingestSealedChunk(kStory, makeLargeChunk(i)); }
    std::string const first_crossing = testing::internal::GetCapturedStdout();
    EXPECT_EQ(countOccurrences(first_crossing, kCapWarning), 1u);

    // W catches up and frees everything: back under the cap
    shipAll(q, store);
    store.confirmPersisted(kStory, 400);
    ASSERT_EQ(store.retainedChunkCount(kStory), 0u);

    // a new outage crosses again at the third chunk and warns again
    testing::internal::CaptureStdout();
    for(int i = 4; i < 7; i++) { store.ingestSealedChunk(kStory, makeLargeChunk(i)); }
    std::string const second_crossing = testing::internal::GetCapturedStdout();
    EXPECT_EQ(countOccurrences(second_crossing, kCapWarning), 1u);
}
