// Unit tests for the orphan-recovery mechanism that backs
// KeeperDataStore::sealOrphanedEvents(): the IngestionQueue helpers
// (orphanStoryIds / extractOrphansForStory) plus the end-to-end flow of
// sealing un-rehomable orphan events into a StoryChunk handed to the
// extraction queue for archival (instead of being dropped).

#include <gtest/gtest.h>

#include <deque>
#include <string>

#include <chrono_monitor.h>
#include <StoryChunk.h>

#include <IngestionQueue.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

static void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "orphan_recovery_test_logger");
        done = true;
    }
}

static chl::LogEvent makeEvent(chl::StoryId sid, uint64_t t, uint32_t idx, std::string const& rec)
{
    return chl::LogEvent(sid, t, /*clientId*/ 1, idx, rec);
}

// An event for a story with no registered ingestion handle becomes an orphan
// and is reported by orphanStoryIds().
TEST(KeeperOrphanRecovery, UnroutableEventsBecomeOrphans)
{
    ensureLogger();
    chl::IngestionQueue iq;
    iq.ingestLogEvent(makeEvent(7, 1000, 0, "a"));
    iq.ingestLogEvent(makeEvent(7, 1001, 1, "b"));

    EXPECT_FALSE(iq.is_empty());
    auto ids = iq.orphanStoryIds();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(*ids.begin(), 7u);
}

// extractOrphansForStory returns and removes exactly one story's orphans.
TEST(KeeperOrphanRecovery, ExtractOrphansForStoryIsolatesAndRemoves)
{
    ensureLogger();
    chl::IngestionQueue iq;
    iq.ingestLogEvent(makeEvent(11, 1000, 0, "s1a"));
    iq.ingestLogEvent(makeEvent(22, 1000, 0, "s2a"));
    iq.ingestLogEvent(makeEvent(11, 1001, 1, "s1b"));

    EXPECT_EQ(iq.orphanStoryIds().size(), 2u);

    std::deque<chl::LogEvent> s1 = iq.extractOrphansForStory(11);
    ASSERT_EQ(s1.size(), 2u);
    EXPECT_EQ(s1[0].getRecord(), "s1a");
    EXPECT_EQ(s1[1].getRecord(), "s1b");

    // story 22's orphan is untouched
    auto remaining = iq.orphanStoryIds();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(*remaining.begin(), 22u);
    // extracting an absent story yields nothing
    EXPECT_TRUE(iq.extractOrphansForStory(99).empty());
}

// The full recovery flow (what sealOrphanedEvents does): pull a retired story's
// orphans, seal them into a named StoryChunk, and stash it to the extraction
// queue -- so they are archived rather than dropped. Afterwards no orphan
// remains.
TEST(KeeperOrphanRecovery, OrphansAreSealedIntoExtractionQueue)
{
    ensureLogger();
    chl::IngestionQueue iq;
    chl::StoryChunkExtractionQueue eq;
    chl::StoryId sid = 7;

    iq.ingestLogEvent(makeEvent(sid, 1000, 0, "late-0"));
    iq.ingestLogEvent(makeEvent(sid, 1002, 1, "late-1"));
    iq.ingestLogEvent(makeEvent(sid, 1001, 2, "late-2"));

    // ---- replicate KeeperDataStore::sealOrphanedEvents' core for story sid ----
    std::deque<chl::LogEvent> orphans = iq.extractOrphansForStory(sid);
    ASSERT_EQ(orphans.size(), 3u);
    uint64_t min_time = orphans.front().time(), max_time = orphans.front().time();
    for(auto const& e: orphans)
    {
        min_time = std::min(min_time, e.time());
        max_time = std::max(max_time, e.time());
    }
    chl::StoryChunk* chunk = new chl::StoryChunk("chron", "story", sid, min_time, max_time + 1, orphans.size());
    for(auto const& e: orphans)
    { chunk->insertEvent(e); }
    eq.stashStoryChunk(chunk);
    // -------------------------------------------------------------------------

    // orphan queue is now drained
    EXPECT_TRUE(iq.is_empty());
    EXPECT_TRUE(iq.orphanStoryIds().empty());

    // the extraction queue holds one chunk with all 3 events (sorted by time)
    ASSERT_EQ(eq.size(), 1);
    chl::StoryChunk* stashed = eq.ejectStoryChunk();
    ASSERT_NE(stashed, nullptr);
    EXPECT_EQ(stashed->getStoryId(), sid);
    EXPECT_EQ(stashed->getEventCount(), 3);
    std::vector<chl::Event> series;
    stashed->extractEventSeries(series);
    ASSERT_EQ(series.size(), 3u);
    EXPECT_EQ(series[0].log_record(), "late-0"); // t=1000
    EXPECT_EQ(series[1].log_record(), "late-2"); // t=1001
    EXPECT_EQ(series[2].log_record(), "late-1"); // t=1002
    delete stashed;
}
