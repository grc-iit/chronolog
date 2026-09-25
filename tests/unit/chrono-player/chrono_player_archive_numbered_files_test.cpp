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
#include <fstream>
#include <list>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
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
    void writeWindow(std::vector<uint64_t> const& event_times,
                     std::string const& chronicle = "chron",
                     std::string const& story = "story")
    {
        chl::StoryChunk window(chronicle, story, kStory, kWindowStart, kWindowEnd);
        uint32_t index = 0;
        for(uint64_t const time: event_times)
        {
            window.insertEvent(chl::LogEvent(kStory, time, 5, index++, "payload"));
        }
        chl::StoryChunkWriter writer(archiveDir.string(), "story_chunks", "data");
        ASSERT_GT(writer.writeStoryChunk(window), 0u);
    }

    // The event times a replay of [start, end) reads from the archive, and
    // through status the code the reader returned for the range.
    std::vector<uint64_t> replayedTimes(uint64_t start,
                                        uint64_t end,
                                        int* status = nullptr,
                                        std::string const& chronicle = "chron",
                                        std::string const& story = "story")
    {
        chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
        archive.initialize();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::list<chl::StoryChunk*> chunks;
        int const read_status = archive.readArchivedStory(chronicle, story, start, end, chunks);
        if(status != nullptr)
        {
            *status = read_status;
        }
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

TEST_F(ArchiveNumberedFiles, AFileThatCannotBeReadIsReported)
{
    writeWindow({kWindowStart + 1});
    // a second write of the window that never completed: the name is right, the
    // content is not
    std::ofstream(archiveDir / "chron.story.60.vlen.1.h5") << "not an HDF5 file";

    int status = chl::CL_SUCCESS;
    EXPECT_EQ(replayedTimes(kWindowStart, kWindowEnd, &status), (std::vector<uint64_t>{kWindowStart + 1}));
    EXPECT_NE(status, chl::CL_SUCCESS);
}

TEST_F(ArchiveNumberedFiles, ReadableFilesReportSuccess)
{
    writeWindow({kWindowStart + 1});
    writeWindow({kWindowStart + 2});

    int status = -1;
    replayedTimes(kWindowStart, kWindowEnd, &status);
    EXPECT_EQ(status, chl::CL_SUCCESS);
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

// ---- names with dots -----------------------------------------------------------
//
// Chronicle and story names may contain dots -- a chronicle named after a fully
// qualified host name, say -- and the file name joins them with dots too. The
// reader takes the start second, "vlen" and the file number from the right end
// of the name, and looks the rest up as one string rather than splitting it.

TEST(ArchiveFileNames, FixedFieldsAreReadFromTheRight)
{
    chl::HDF5ArchiveReadingAgent::ArchiveFileName parsed;
    ASSERT_TRUE(
            chl::HDF5ArchiveReadingAgent::parseArchiveFileName("/a/node01.cluster.local.cpu.usage.60.vlen.h5", parsed));
    EXPECT_EQ(parsed.story_prefix, "node01.cluster.local.cpu.usage");
    EXPECT_EQ(parsed.start_time, 60 * NS);
    EXPECT_FALSE(parsed.numbered);

    ASSERT_TRUE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName("run.b.123.60.vlen.2.h5", parsed));
    EXPECT_EQ(parsed.story_prefix, "run.b.123");
    EXPECT_EQ(parsed.start_time, 60 * NS);
    EXPECT_TRUE(parsed.numbered);

    // a name part that looks like the suffix stays in the prefix
    ASSERT_TRUE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName("c.x.5.vlen.60.vlen.h5", parsed));
    EXPECT_EQ(parsed.story_prefix, "c.x.5.vlen");
    EXPECT_EQ(parsed.start_time, 60 * NS);
}

TEST(ArchiveFileNames, OtherFilesAreNotArchiveFiles)
{
    chl::HDF5ArchiveReadingAgent::ArchiveFileName parsed;
    EXPECT_FALSE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName("notes.txt", parsed));
    EXPECT_FALSE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName("c.s.60.vlen.h5.partial.host.12.3", parsed));
    EXPECT_FALSE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName("c.s.sixty.vlen.h5", parsed));
    EXPECT_FALSE(chl::HDF5ArchiveReadingAgent::parseArchiveFileName(".60.vlen.h5", parsed));
}

TEST_F(ArchiveNumberedFiles, AStoryWithDotsInItsNamesIsReplayedWithEveryWrite)
{
    std::string const chronicle = "node01.cluster.local";
    std::string const story = "cpu.usage";
    writeWindow({kWindowStart + 1, kWindowStart + 2}, chronicle, story);
    writeWindow({kWindowStart + 3}, chronicle, story); // a later write: .vlen.1.h5

    EXPECT_EQ(replayedTimes(kWindowStart, kWindowEnd, nullptr, chronicle, story),
              (std::vector<uint64_t>{kWindowStart + 1, kWindowStart + 2, kWindowStart + 3}));
}

TEST_F(ArchiveNumberedFiles, ANumericNamePartIsNotTakenForTheStartTime)
{
    writeWindow({kWindowStart + 1}, "run", "b.123");

    EXPECT_EQ(replayedTimes(kWindowStart, kWindowEnd, nullptr, "run", "b.123"),
              (std::vector<uint64_t>{kWindowStart + 1}));
    // and the file is not taken for story "b"
    EXPECT_TRUE(replayedTimes(kWindowStart, kWindowEnd, nullptr, "run", "b").empty());
}
