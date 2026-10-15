// A replay splits at B, the highest watermark the story's keepers report: the
// archive serves [start, B) and the keepers the rest. A watermark says the
// grapher has written the events below it; the player finds the files through
// the archive manifest, which the grapher appends to before it reports. A
// keeper that frees a chunk right after the report must not leave a replay
// without the chunk's events.
//
// These tests run a replay through the real keeper retention store, replay
// split, archive reader and merge.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <list>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <HDF5ArchiveReadingAgent.h>
#include <HotRangeSplit.h>
#include <KeeperChunkRetentionStore.h>
#include <ReplayEventMerge.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryChunkWriter.h>

#include "ArchiveTestSupport.h"

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t NS = 1000000000ULL;
constexpr chl::StoryId kStory = 7;
constexpr uint64_t kChunkStart = 10 * NS;
constexpr uint64_t kChunkEnd = 11 * NS;
constexpr uint64_t kEventTime = kChunkStart + 1;
constexpr uint64_t kReplayStart = 0;
constexpr uint64_t kReplayEnd = 20 * NS;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_visibility_test_logger");
        done = true;
    }
}

class ArchiveVisibility: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir =
                fs::temp_directory_path() / ("chronolog_archive_visibility_test_" + std::to_string(::getpid()) + "_" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
    }

    void TearDown() override { fs::remove_all(archiveDir); }

    // A player whose archive index was built before the grapher writes.
    void startPlayerArchive(chl::HDF5ArchiveReadingAgent& archive) { archive.initialize(); }

    // One chunk sealed on the keeper, shipped, written to the archive by the
    // grapher, and covered by the grapher's watermark report.
    void sealShipWriteAndReport(chl::KeeperChunkRetentionStore& keeper)
    {
        auto* chunk = new chl::StoryChunk("chron", "story", kStory, kChunkStart, kChunkEnd);
        chunk->insertEvent(chl::LogEvent(kStory, kEventTime, 5, 0, "payload"));
        keeper.ingestSealedChunk(kStory, chunk);
        chl::StoryChunk* shipped = extractionQueue.ejectStoryChunk();
        ASSERT_EQ(shipped, chunk);
        ASSERT_FALSE(chl::test::publishWindow(archiveDir, *shipped).empty());
        keeper.markShipped(shipped);
        keeper.confirmPersisted(kStory, kChunkEnd);
    }

    // What PlaybackService and QueryResponseAgent return for the replay window.
    std::vector<chl::Event> replay(chl::KeeperChunkRetentionStore& keeper, chl::HDF5ArchiveReadingAgent& archive)
    {
        std::vector<chl::HotRangeResponse> responses{keeper.fetchRange(kStory, kReplayStart, kReplayEnd, 1000)};
        chl::HotRangeSplit split = chl::splitHotRange(responses, kReplayStart, kReplayEnd);
        std::vector<chl::Event> hot;
        for(auto const& log_event: split.hotEvents)
        {
            hot.push_back(
                    chl::Event{log_event.eventTime, log_event.clientId, log_event.eventIndex, log_event.logRecord});
        }
        std::vector<chl::Event> archived;
        if(!split.complete)
        {
            std::list<chl::StoryChunk*> chunks;
            archive.readArchivedStory("chron", "story", kReplayStart, split.boundary, chunks);
            for(chl::StoryChunk* chunk: chunks)
            {
                chunk->extractEventSeries(archived);
                delete chunk;
            }
        }
        return chl::mergeReplayEvents(std::move(archived), std::move(hot));
    }

    // the archive reader logs thallium::thread::self_id(), which needs Argobots
    thallium::abt argobots;
    fs::path archiveDir;
    // declared before any store, which may hand chunks back to it when destroyed
    chl::StoryChunkExtractionQueue extractionQueue;
};
} // namespace

TEST_F(ArchiveVisibility, ChunkWrittenBeforeThePlayerSeesTheFileIsStillReplayed)
{
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    startPlayerArchive(archive);
    chl::KeeperChunkRetentionStore keeper(extractionQueue, 100, 0, false, std::chrono::seconds(60));
    sealShipWriteAndReport(keeper);

    std::vector<chl::Event> events = replay(keeper, archive);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().time(), kEventTime);
    EXPECT_EQ(events.front().log_record(), "payload");
    archive.shutdown();
}

TEST_F(ArchiveVisibility, KeeperFreesTheChunkOnceThePlayerCanReadTheFile)
{
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    startPlayerArchive(archive);
    chl::KeeperChunkRetentionStore keeper(extractionQueue, 100, 0, false, std::chrono::milliseconds(600));
    sealShipWriteAndReport(keeper);
    keeper.releaseStoryTail(kStory);
    EXPECT_EQ(keeper.freeDurableChunks(), 0u);
    ASSERT_EQ(keeper.retainedChunkCount(kStory), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    EXPECT_EQ(keeper.freeDurableChunks(), 1u);
    EXPECT_EQ(keeper.retainedChunkCount(kStory), 0u);

    std::vector<chl::Event> events = replay(keeper, archive);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().time(), kEventTime);
    archive.shutdown();
}
