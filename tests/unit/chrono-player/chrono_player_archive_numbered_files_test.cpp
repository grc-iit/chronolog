// A grapher writes each window of a story to
// {chronicle}.{story}.{start second}.vlen.h5. When the same window is written
// again, the file gets a number: .vlen.1.h5, .vlen.2.h5. That happens when
// keeper chunks for a window arrive after the grapher has already merged and
// written it: a keeper that was late, a re-sent chunk, or a story whose
// pipeline retired before the last chunks arrived. Once the keepers free those
// chunks, the numbered file is the only copy of their events, so a replay has
// to read every write of a window.

#include <gtest/gtest.h>

#include <algorithm>
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
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t NS = 1000000000ULL;
constexpr chl::StoryId kStory = 7;
// file names carry the window start in whole seconds
constexpr uint64_t kWindowStart = 60 * NS;
constexpr uint64_t kWindowEnd = 120 * NS;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_numbered_files_test_logger");
        done = true;
    }
}

class ArchiveNumberedFiles: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir =
                fs::temp_directory_path() / ("chronolog_archive_numbered_files_test_" + std::to_string(::getpid()) +
                                             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
    }

    void TearDown() override { fs::remove_all(archiveDir); }

    // Writes the window with events at the given times, as the grapher's HDF5
    // extractor does; a second write of the window gets a numbered file.
    void writeWindow(std::vector<uint64_t> const& event_times)
    {
        chl::StoryChunk window("chron", "story", kStory, kWindowStart, kWindowEnd);
        uint32_t index = 0;
        for(uint64_t const time: event_times)
        {
            window.insertEvent(chl::LogEvent(kStory, time, 5, index++, "payload"));
        }
        chl::StoryChunkWriter writer(archiveDir.string(), "story_chunks", "data");
        ASSERT_GT(writer.writeStoryChunk(window), 0u);
    }

    // The event times a replay of [start, end) reads from the archive.
    std::vector<uint64_t> replayedTimes(uint64_t start, uint64_t end)
    {
        chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
        archive.initialize();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
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

TEST_F(ArchiveNumberedFiles, ReplayReadsEveryWriteOfAWindow)
{
    writeWindow({kWindowStart + 1});
    writeWindow({kWindowStart + 2});
    writeWindow({kWindowStart + 3});
    ASSERT_TRUE(fs::exists(archiveDir / "chron.story.60.vlen.2.h5"));

    EXPECT_EQ(replayedTimes(kWindowStart, kWindowEnd),
              (std::vector<uint64_t>{kWindowStart + 1, kWindowStart + 2, kWindowStart + 3}));
}

TEST_F(ArchiveNumberedFiles, ReplayReadsLaterWritesOfTheWindowThatReachesPastTheRange)
{
    // the first write has an event after the end of the replay, the second
    // write has one inside it
    writeWindow({kWindowStart + 1, kWindowStart + 40 * NS});
    writeWindow({kWindowStart + 2});

    EXPECT_EQ(replayedTimes(kWindowStart, kWindowStart + 30 * NS),
              (std::vector<uint64_t>{kWindowStart + 1, kWindowStart + 2}));
}
