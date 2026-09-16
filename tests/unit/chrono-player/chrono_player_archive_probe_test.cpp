// The player learns of new archive files by listing the archive directory every
// few seconds. On a shared file system that listing is cached: a file the
// grapher wrote on another node can stay invisible for as long as the client
// caches directory attributes, which is why keepers hold a written chunk for
// archive_visibility_delay_secs before freeing it.
//
// Looking a file up by name does not go through that cache, so a replay that
// reaches past the newest file the player has listed probes the names the
// grapher would have used for the windows in between. These tests give the
// agent a scan interval far longer than the test, so a file it finds can only
// have come from a probe.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <list>
#include <string>
#include <vector>
#include <unistd.h>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <HDF5ArchiveReadingAgent.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t NS = 1000000000ULL;
constexpr chl::StoryId kStory = 7;
// the grapher's windows in these tests, matching the template's 30 s
constexpr uint64_t kWindowSecs = 30;
constexpr auto kScanNeverRuns = std::chrono::milliseconds(600000);

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_probe_test_logger");
        done = true;
    }
}

class ArchiveProbe: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir = fs::temp_directory_path() / ("chronolog_archive_probe_test_" + std::to_string(::getpid()) + "_" +
                                                  ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
    }

    void TearDown() override { fs::remove_all(archiveDir); }

    // Writes the window starting at that second, as the grapher's extractor does.
    void writeWindow(uint64_t start_secs)
    {
        chl::StoryChunk window("chron", "story", kStory, start_secs * NS, (start_secs + kWindowSecs) * NS);
        window.insertEvent(chl::LogEvent(kStory, start_secs * NS + 1, 5, 0, "payload"));
        chl::StoryChunkWriter writer(archiveDir.string(), "story_chunks", "data");
        ASSERT_GT(writer.writeStoryChunk(window), 0u);
    }

    std::vector<uint64_t> replayedTimes(chl::HDF5ArchiveReadingAgent& archive, uint64_t start, uint64_t end)
    {
        std::list<chl::StoryChunk*> chunks;
        archive.readArchivedStory("chron", "story", start, end, chunks);
        std::vector<uint64_t> times;
        for(chl::StoryChunk* chunk: chunks)
        {
            for(auto event = chunk->begin(); event != chunk->end(); ++event) { times.push_back(event->second.time()); }
            delete chunk;
        }
        std::sort(times.begin(), times.end());
        return times;
    }

    // the archive reader logs thallium::thread::self_id(), which needs Argobots
    thallium::abt argobots;
    fs::path archiveDir;
};
} // namespace

TEST_F(ArchiveProbe, AWindowWrittenAfterTheLastScanIsStillReplayed)
{
    writeWindow(60);
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string(), true, kScanNeverRuns, kWindowSecs);
    archive.initialize();

    // written after the agent listed the directory, and no scan will run again
    writeWindow(90);

    EXPECT_EQ(replayedTimes(archive, 60 * NS, 120 * NS), (std::vector<uint64_t>{60 * NS + 1, 90 * NS + 1}));
}

TEST_F(ArchiveProbe, AStoryWhoseFirstWindowArrivedAfterTheScanIsReplayed)
{
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string(), true, kScanNeverRuns, kWindowSecs);
    archive.initialize();

    writeWindow(60);

    EXPECT_EQ(replayedTimes(archive, 60 * NS, 90 * NS), (std::vector<uint64_t>{60 * NS + 1}));
}

TEST_F(ArchiveProbe, ARangeTheListingAlreadyCoversStillReads)
{
    writeWindow(60);
    writeWindow(90);
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string(), true, kScanNeverRuns, kWindowSecs);
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 60 * NS, 120 * NS), (std::vector<uint64_t>{60 * NS + 1, 90 * NS + 1}));
}
