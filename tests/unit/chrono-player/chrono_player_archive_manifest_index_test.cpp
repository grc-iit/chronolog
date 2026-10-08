// The player finds a story's archive files through the archive manifest: every
// grapher appends a record for each file it publishes and for each story or
// chronicle whose files it deletes. A replay reads what the logs gained since the
// last replay before it looks anything up, so a file is replayable as soon as its
// record is appended, with no directory listing to wait for.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <list>
#include <string>
#include <vector>
#include <unistd.h>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <HDF5ArchiveReadingAgent.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

#include "ArchiveTestSupport.h"

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t NS = 1000000000ULL;
constexpr chl::StoryId kStory = 7;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_manifest_index_test_logger");
        done = true;
    }
}

chl::StoryChunk window(uint64_t start_secs,
                       uint64_t end_secs,
                       std::vector<uint64_t> const& event_times,
                       std::string const& chronicle = "chron",
                       std::string const& story = "story")
{
    chl::StoryChunk chunk(chronicle, story, kStory, start_secs * NS, end_secs * NS);
    uint32_t index = 0;
    for(uint64_t const time: event_times) { chunk.insertEvent(chl::LogEvent(kStory, time, 5, index++, "payload")); }
    return chunk;
}

class ArchiveManifestIndex: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir =
                fs::temp_directory_path() / ("chronolog_archive_manifest_index_test_" + std::to_string(::getpid()) +
                                             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
    }

    void TearDown() override { fs::remove_all(archiveDir); }

    std::string publish(chl::StoryChunk chunk, std::string const& writer = "1")
    {
        std::string const file = chl::test::publishWindow(archiveDir, chunk, writer);
        EXPECT_FALSE(file.empty());
        return file;
    }

    std::vector<uint64_t> replayedTimes(chl::HDF5ArchiveReadingAgent& archive,
                                        uint64_t start,
                                        uint64_t end,
                                        int* status = nullptr,
                                        std::string const& chronicle = "chron",
                                        std::string const& story = "story")
    {
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

TEST_F(ArchiveManifestIndex, AWindowPublishedBeforeStartupIsReplayed)
{
    publish(window(60, 90, {60 * NS + 1, 60 * NS + 2}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1, 60 * NS + 2}));
}

TEST_F(ArchiveManifestIndex, AWindowPublishedAfterStartupIsReplayedAtOnce)
{
    publish(window(60, 90, {60 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 1u);

    publish(window(90, 120, {90 * NS + 1}));
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1, 90 * NS + 1}));
}

TEST_F(ArchiveManifestIndex, AStoryWhoseFirstWindowCameAfterStartupIsReplayed)
{
    // the player starts before any grapher has written anything
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS).empty());

    publish(window(60, 90, {60 * NS + 1}));
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1}));
}

// A window written again after the story has moved on -- a late or re-sent
// keeper chunk -- starts before the newest file the player knows. Name probing
// only looked past that newest file, so it missed this one.
TEST_F(ArchiveManifestIndex, ALateWriteOfAnOldWindowIsReplayed)
{
    publish(window(60, 90, {60 * NS + 1}));
    publish(window(90, 120, {90 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 2u);

    publish(window(60, 90, {60 * NS + 7}));
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1, 60 * NS + 7, 90 * NS + 1}));
}

// A salvage file covers one keeper chunk's range, which need not line up with
// the grapher's windows.
TEST_F(ArchiveManifestIndex, AFileStartingBeforeTheRangeButReachingIntoItIsRead)
{
    publish(window(55, 65, {61 * NS}));
    publish(window(60, 90, {70 * NS}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 60 * NS, 90 * NS), (std::vector<uint64_t>{61 * NS, 70 * NS}));
    EXPECT_EQ(replayedTimes(archive, 66 * NS, 90 * NS), (std::vector<uint64_t>{70 * NS}));
}

// A replay starts its search at most the story's longest range before the
// replay's start: a long range is still found from far away.
TEST_F(ArchiveManifestIndex, ALongRangeStartingFarBeforeTheReplayIsRead)
{
    publish(window(0, 1000, {650 * NS}));
    for(uint64_t start = 30; start < 990; start += 30) { publish(window(start, start + 30, {start * NS + 1})); }
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 640 * NS, 655 * NS), (std::vector<uint64_t>{650 * NS}));
    EXPECT_EQ(replayedTimes(archive, 1000 * NS, 2000 * NS).size(), 0u);
}

// A log is read in pieces. A piece holding nothing readable must not end the
// refresh early and leave a record behind it for some later replay.
TEST_F(ArchiveManifestIndex, ARecordBehindMoreThanAPieceOfUnreadableLinesIsFoundAtOnce)
{
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    fs::create_directories(archiveDir / chl::kArchiveManifestDirName);
    {
        std::ofstream log(archiveDir / chl::kArchiveManifestDirName / "1.log", std::ios::app | std::ios::binary);
        std::string const junk(1023, 'x');
        for(int i = 0; i < 5 * 1024; ++i) { log << junk << '\n'; } // 5 MiB
    }
    publish(window(60, 90, {60 * NS + 1}));

    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1}));
}

TEST_F(ArchiveManifestIndex, EveryGraphersLogIsRead)
{
    publish(window(60, 90, {60 * NS + 1}), "1");
    publish(window(90, 120, {90 * NS + 1}), "2");
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1, 90 * NS + 1}));
}

TEST_F(ArchiveManifestIndex, OtherStoriesAreNotReturned)
{
    publish(window(60, 90, {60 * NS + 1}, "chron", "story"));
    publish(window(60, 90, {60 * NS + 2}, "chron", "other"));
    publish(window(60, 90, {60 * NS + 3}, "chron.story", "x"));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1}));
}

TEST_F(ArchiveManifestIndex, AStoryWithNothingArchivedReadsEmptyRatherThanFailing)
{
    publish(window(60, 90, {60 * NS + 1}, "chron", "other"));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    int status = -1;
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, &status).empty());
    EXPECT_EQ(status, chl::CL_SUCCESS);
}

TEST_F(ArchiveManifestIndex, AnArchiveDirectoryThatDoesNotExistIsNotReportedComplete)
{
    // misconfigured archive path: the player cannot tell what is archived, so
    // a story it cannot find must not read as "nothing archived"
    chl::HDF5ArchiveReadingAgent archive((archiveDir / "no_such_directory").string());
    archive.initialize();

    int status = chl::CL_SUCCESS;
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, &status).empty());
    EXPECT_NE(status, chl::CL_SUCCESS);
}

TEST_F(ArchiveManifestIndex, AFileThatCannotBeReadIsReported)
{
    publish(window(60, 90, {60 * NS + 1}));
    std::string const second = publish(window(60, 90, {60 * NS + 2}));
    std::ofstream(second, std::ios::trunc) << "not an HDF5 file";
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    int status = chl::CL_SUCCESS;
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, &status), (std::vector<uint64_t>{60 * NS + 1}));
    EXPECT_NE(status, chl::CL_SUCCESS);
}

// ---- deletions --------------------------------------------------------------

TEST_F(ArchiveManifestIndex, ADestroyedStoryIsNotReplayed)
{
    std::string const file = publish(window(60, 90, {60 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 1u);

    fs::remove(file);
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story"));
    int status = -1;
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, &status).empty());
    EXPECT_EQ(status, chl::CL_SUCCESS);
}

// Destroy, create again under the same name, write the same window: the new
// file can take the old file's name. Only the new events may come back, and a
// player that starts afterwards must agree with one that was running.
TEST_F(ArchiveManifestIndex, AStoryCreatedAgainAfterADestroyReplaysOnlyItsNewEvents)
{
    std::string const old_file = publish(window(60, 90, {60 * NS + 1}));
    chl::HDF5ArchiveReadingAgent running(archiveDir.string());
    running.initialize();
    ASSERT_EQ(replayedTimes(running, 0, 200 * NS).size(), 1u);

    fs::remove(old_file);
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story"));
    publish(window(60, 90, {60 * NS + 9}));

    EXPECT_EQ(replayedTimes(running, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 9}));
    chl::HDF5ArchiveReadingAgent restarted(archiveDir.string());
    restarted.initialize();
    EXPECT_EQ(replayedTimes(restarted, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 9}));
}

TEST_F(ArchiveManifestIndex, ADestroyedChronicleTakesAllItsStories)
{
    std::string const a = publish(window(60, 90, {60 * NS + 1}, "chron", "story"));
    std::string const b = publish(window(60, 90, {60 * NS + 2}, "chron", "other"));
    publish(window(60, 90, {60 * NS + 3}, "kept", "story"));
    fs::remove(a);
    fs::remove(b);
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", ""));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, nullptr, "chron", "story").empty());
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, nullptr, "chron", "other").empty());
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, nullptr, "kept", "story").size(), 1u);
}

// A chronicle's deletion reaches all its stories, the one with an empty name
// too, and no chronicle whose name sorts next to it.
TEST_F(ArchiveManifestIndex, ADestroyedChronicleLeavesChroniclesThatSortNextToIt)
{
    publish(window(60, 90, {60 * NS + 1}, "chron", ""));
    publish(window(60, 90, {60 * NS + 2}, "chron", "story"));
    publish(window(60, 90, {60 * NS + 3}, "chro", "story"));
    publish(window(60, 90, {60 * NS + 4}, "chron2", "story"));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();

    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", ""));
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, nullptr, "chron", "").empty());
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, nullptr, "chron", "story").empty());
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, nullptr, "chro", "story").size(), 1u);
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, nullptr, "chron2", "story").size(), 1u);
}

// A grapher records a deletion after its earlier records, so everything its
// log named for the story before it is gone, whatever a lookup says: on a
// shared file system a cached lookup can still find a deleted file.
TEST_F(ArchiveManifestIndex, ADeletionDropsTheSameGraphersEarlierFilesWithoutLookingThemUp)
{
    publish(window(60, 90, {60 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 1u);

    // the file stays, standing in for a lookup answered from a stale cache
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story"));
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS).empty());
}

// Each grapher records a destroy in its own log, so another grapher's
// deletion leaves this grapher's files to this grapher's own deletion, and
// looks nothing up: on NFS a lookup made just after a destroy caches "not
// found" and can hide a file the story, created again, writes under that name.
TEST_F(ArchiveManifestIndex, AnotherGraphersDeletionLeavesThisGraphersFilesToItsOwn)
{
    std::string const file = publish(window(60, 90, {60 * NS + 1}), "2");
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 1u);

    // the file is gone when grapher 1's deletion is read, yet stays indexed
    fs::path const kept = archiveDir.parent_path() / (archiveDir.filename().string() + "_kept.h5");
    fs::rename(file, kept);
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story", "1"));
    int status = -1;
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS, &status).empty());
    EXPECT_EQ(status, chl::CL_SUCCESS);
    fs::rename(kept, file);
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1}));

    // grapher 2's own deletion drops it, file or no file
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story", "2"));
    EXPECT_TRUE(replayedTimes(archive, 0, 200 * NS).empty());
}

// File names are never reused, but one file can still be recorded by two logs
// (a rebuilt manifest, for one). A deletion takes only its own log's claim, so
// the file stays until every log that recorded it has let go of it.
TEST_F(ArchiveManifestIndex, AFileTwoLogsRecordedStaysUntilBothLetGo)
{
    std::string const file = publish(window(60, 90, {60 * NS + 1}), "2");
    {
        chl::ArchiveManifestWriter other(archiveDir.string(), "1");
        ASSERT_EQ(other.open(), chl::CL_SUCCESS);
        chl::ArchiveManifestRecord record;
        record.op = chl::ArchiveManifestRecord::Op::PUBLISH;
        record.chronicle = "chron";
        record.story = "story";
        record.file = fs::path(file).lexically_relative(archiveDir).string();
        record.start = 60 * NS;
        record.end = 90 * NS;
        record.events = 1;
        ASSERT_EQ(other.append(record), chl::CL_SUCCESS);
    }
    chl::HDF5ArchiveReadingAgent running(archiveDir.string());
    running.initialize();

    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story", "1"));
    EXPECT_EQ(replayedTimes(running, 0, 200 * NS), (std::vector<uint64_t>{60 * NS + 1}));
    ASSERT_TRUE(chl::test::recordDeletion(archiveDir, "chron", "story", "2"));
    EXPECT_TRUE(replayedTimes(running, 0, 200 * NS).empty());
}

// A manifest log the player cannot read may name files of the range: the
// replay is not reported complete.
TEST_F(ArchiveManifestIndex, AManifestLogThatCannotBeReadIsReported)
{
    publish(window(60, 90, {60 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    std::string const log = chl::listArchiveManifestLogs(archiveDir.string()).front();
    fs::permissions(log, fs::perms::none);
    if(::access(log.c_str(), R_OK) == 0)
    {
        GTEST_SKIP() << "permissions do not apply (running as root)";
    }

    int status = chl::CL_SUCCESS;
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, &status).size(), 1u);
    EXPECT_NE(status, chl::CL_SUCCESS);
    fs::permissions(log, fs::perms::owner_read | fs::perms::owner_write);
}

// The deletion's record can arrive after the files are gone.
TEST_F(ArchiveManifestIndex, AFileDeletedBeforeItsRecordIsSkippedWithoutAnError)
{
    std::string const gone = publish(window(60, 90, {60 * NS + 1}));
    publish(window(90, 120, {90 * NS + 1}));
    chl::HDF5ArchiveReadingAgent archive(archiveDir.string());
    archive.initialize();
    ASSERT_EQ(replayedTimes(archive, 0, 200 * NS).size(), 2u);

    fs::remove(gone);
    int status = -1;
    EXPECT_EQ(replayedTimes(archive, 0, 200 * NS, &status), (std::vector<uint64_t>{90 * NS + 1}));
    EXPECT_EQ(status, chl::CL_SUCCESS);
}
