// How the Player builds its archive index.
//
// Until now it discovered archive content by walking the directory tree at
// startup, which is O(archive) on every Player restart and yields only what a file
// name can express -- chronicle, story, and the window START. The manifest gives
// the same index as one sequential read, plus each file's window END, which is
// what lets a read skip a file without opening it.
//
// Losing the manifest must degrade performance, not correctness, so the recursive
// scan stays as the fallback for archives written before it existed. These tests
// pin both paths down, and use a file that is on disk but absent from the manifest
// to tell them apart: the scan finds it, a manifest-backed load does not.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <list>
#include <string>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#include <ArchiveManifest.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

#include <HDF5ArchiveReadingAgent.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t NS = 1000000000ULL;
constexpr uint64_t BASE = 1736800000ULL * NS;
constexpr chl::StoryId STORY_ID = 4242;

class ArchiveIndexTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() / ("chronolog_archive_index_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // Writes a real HDF5 file for [start, end) and returns its base name.
    std::string writeChunk(uint64_t start, uint64_t end, int events = 3)
    {
        chl::StoryChunk chunk("c1", "s1", STORY_ID, start, end);
        for(int i = 0; i < events; i++)
        {
            chunk.insertEvent(chl::LogEvent(STORY_ID, start + (uint64_t)(i + 1), 7, (chl::chrono_index)i, "payload"));
        }
        chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
        chl::StoryChunkWriteResult const result = writer.writeStoryChunk(chunk);
        EXPECT_GT(result.file_size, 0u);
        return fs::path(result.file_name).filename().string();
    }

    void recordInManifest(std::string const& file, uint64_t start, uint64_t end)
    {
        recordInManifestFor("", file, start, end);
    }

    // writer == "" is the legacy unsuffixed log; anything else is one Grapher's own.
    void recordInManifestFor(std::string const& writer, std::string const& file, uint64_t start, uint64_t end)
    {
        chl::ArchiveManifest manifest(root.string(), false, writer);
        chl::ArchiveManifestRecord record;
        record.chronicle = "c1";
        record.story = "s1";
        record.story_id = STORY_ID;
        record.file = file;
        record.start = start;
        record.end = end;
        record.state = chl::ManifestState::PUBLISHED;
        manifest.append(record);
    }

    size_t eventsInRange(chl::HDF5ArchiveReadingAgent& agent, uint64_t from, uint64_t to)
    {
        std::list<chl::StoryChunk*> chunks;
        agent.readArchivedStory("c1", "s1", from, to, chunks);
        size_t count = 0;
        for(chl::StoryChunk* chunk: chunks)
        {
            count += chunk->getEventCount();
            delete chunk;
        }
        return count;
    }

    fs::path root;
};

} // namespace

TEST_F(ArchiveIndexTest, IndexIsBuiltFromTheManifestWithoutScanningTheDirectory)
{
    std::string const listed = writeChunk(BASE, BASE + 10 * NS);
    // On disk but absent from the manifest. A directory scan would index it; a
    // manifest-backed load must not, which is what distinguishes the two paths.
    std::string const unlisted = writeChunk(BASE + 100 * NS, BASE + 110 * NS);
    recordInManifest(listed, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_GT(eventsInRange(agent, BASE, BASE + 10 * NS), 0u) << "the manifest-listed file should be readable";
    EXPECT_EQ(eventsInRange(agent, BASE + 100 * NS, BASE + 110 * NS), 0u)
            << "a file the manifest does not list was indexed, so the scan ran: " << unlisted;

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, FallsBackToScanningWhenThereIsNoManifest)
{
    // An archive written before the manifest existed still has to be readable.
    writeChunk(BASE, BASE + 10 * NS);
    ASSERT_FALSE(fs::exists(root / "archive_manifest.log"));

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_GT(eventsInRange(agent, BASE, BASE + 10 * NS), 0u) << "the scan fallback did not index an existing archive";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, FallsBackToScanningWhenTheManifestIsUnreadable)
{
    writeChunk(BASE, BASE + 10 * NS);
    // Nothing in it parses, so it yields no records -- same as having none.
    {
        std::ofstream corrupt(root / "archive_manifest.log");
        corrupt << "this is not a manifest\nnor is this\n";
    }

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_GT(eventsInRange(agent, BASE, BASE + 10 * NS), 0u)
            << "a corrupt manifest must degrade to a scan, not to an empty index";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, ADeletedFileIsNotIndexed)
{
    std::string const file = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(file, BASE, BASE + 10 * NS);
    {
        chl::ArchiveManifest manifest(root.string());
        chl::ArchiveManifestRecord removal;
        removal.chronicle = "c1";
        removal.story = "s1";
        removal.story_id = STORY_ID;
        removal.file = file;
        removal.state = chl::ManifestState::DELETED;
        manifest.append(removal);
    }
    // The record supersedes the publication even though the bytes are still there,
    // so the index must not name it.
    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    agent.initialize();

    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 0u);

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, AWindowEndingBeforeTheQueryIsNotRead)
{
    // The manifest supplies each file's end, so a file whose window closes before
    // the requested start is skipped outright. Correctness is unchanged either
    // way -- readStoryChunkFile filters events too -- but the file is not opened.
    std::string const early = writeChunk(BASE, BASE + 10 * NS);
    std::string const late = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifest(early, BASE, BASE + 10 * NS);
    recordInManifest(late, BASE + 20 * NS, BASE + 30 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u) << "only the later window should be returned";
    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u) << "only the earlier window should be returned";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, AWindowStartingAtOrAfterTheQueryEndIsNotRead)
{
    std::string const early = writeChunk(BASE, BASE + 10 * NS);
    std::string const late = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifest(early, BASE, BASE + 10 * NS);
    recordInManifest(late, BASE + 20 * NS, BASE + 30 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    // Range stops before the later window opens.
    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 15 * NS), 3u);

    agent.shutdown();
}

// The reading agent's monitoring stream is an Argobots execution stream.
int main(int argc, char** argv)
{
    thallium::abt scope;
    chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "archive_index_test_logger");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// ---- rotated files and de-duplication --------------------------------------
//
// A rotated file ("<...>.vlen.<n>.h5") is never part 2 of a chunk: the writer
// rotates only when the base name is already taken, so it is always a SECOND write
// of the same story and the same window start. Two kinds occur, and until now
// neither default was correct:
//
//   re-send   a keeper stall or grapher restart re-publishes the same window.
//             Reading it returns every event twice.
//   salvage   events that could not be merged into the main window because the
//             timeline had already moved past them. Skipping it silently DROPS
//             those events -- they exist nowhere else.
//
// The manifest lists rotations against the same (chronicle, story, start) key, and
// a window's files are read into one StoryChunk, which is keyed by EventSequence.
// That makes the re-send case collapse and the salvage case union, using the same
// key the hot path already de-duplicates on.

TEST_F(ArchiveIndexTest, ARepublishedWindowIsReturnedOnce)
{
    // Same window, same events, written twice: the second lands as a rotation.
    std::string const first = writeChunk(BASE, BASE + 10 * NS, 3);
    std::string const second = writeChunk(BASE, BASE + 10 * NS, 3);
    ASSERT_NE(first, second) << "the second write should have rotated";
    recordInManifest(first, BASE, BASE + 10 * NS);
    recordInManifest(second, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u) << "a re-sent window was returned more than once";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, SalvagedEventsInARotatedFileAreReturned)
{
    // The salvage case: the rotation holds events the main file never got. They
    // exist nowhere else, so not reading it loses them outright.
    chl::StoryChunk main_chunk("c1", "s1", STORY_ID, BASE, BASE + 10 * NS);
    for(int i = 0; i < 3; i++)
    {
        main_chunk.insertEvent(chl::LogEvent(STORY_ID, BASE + (uint64_t)(i + 1), 7, (chl::chrono_index)i, "main"));
    }
    chl::StoryChunk salvage_chunk("c1", "s1", STORY_ID, BASE, BASE + 10 * NS);
    for(int i = 0; i < 2; i++)
    {
        // Different client id, so these are distinct EventSequences, not repeats.
        salvage_chunk.insertEvent(
                chl::LogEvent(STORY_ID, BASE + (uint64_t)(i + 1), 9, (chl::chrono_index)i, "salvaged"));
    }

    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunkWriteResult const main_result = writer.writeStoryChunk(main_chunk);
    chl::StoryChunkWriteResult const salvage_result = writer.writeStoryChunk(salvage_chunk);
    ASSERT_GT(main_result.file_size, 0u);
    ASSERT_GT(salvage_result.file_size, 0u);
    recordInManifest(fs::path(main_result.file_name).filename().string(), BASE, BASE + 10 * NS);
    recordInManifest(fs::path(salvage_result.file_name).filename().string(), BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 5u)
            << "salvaged events in the rotated file were dropped (3 main + 2 salvaged expected)";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, ARotationDeletedFromTheManifestIsNotRead)
{
    std::string const first = writeChunk(BASE, BASE + 10 * NS, 3);
    std::string const second = writeChunk(BASE, BASE + 10 * NS, 3);
    recordInManifest(first, BASE, BASE + 10 * NS);
    recordInManifest(second, BASE, BASE + 10 * NS);
    {
        chl::ArchiveManifest manifest(root.string());
        chl::ArchiveManifestRecord removal;
        removal.chronicle = "c1";
        removal.story = "s1";
        removal.story_id = STORY_ID;
        removal.file = second;
        removal.state = chl::ManifestState::DELETED;
        manifest.append(removal);
    }

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);

    // Still 3: the surviving base file covers the window on its own.
    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u);

    agent.shutdown();
}

// ---- keeping the index current from the manifest tail ----------------------
//
// The index is built once at startup, but the Grapher keeps publishing. Until now
// the Player noticed by diffing the whole archive directory every 5 s, which costs
// a full walk per tick and scales with the archive rather than with what changed.
//
// The manifest log is append-only, so the same information is a read of the bytes
// past the last offset. These tests drive that read directly rather than waiting on
// the monitoring thread: the interesting cases are byte-offset bookkeeping, and a
// sleep-based test would be both slower and flakier.

TEST_F(ArchiveIndexTest, ManifestRecordsAppendedAfterStartupAreIndexed)
{
    std::string const first = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(first, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tail and the consumed-record counts below are a coin
    // flip. pollManifestTail() does not consult the shutdown flag.
    agent.shutdown();
    ASSERT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 0u) << "not published yet";

    // The Grapher publishes another window while the Player is running.
    std::string const second = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifest(second, BASE + 20 * NS, BASE + 30 * NS);

    EXPECT_GT(agent.pollManifestTail(), 0) << "the new record was not picked up";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u);

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, PollingConsumesEachRecordExactlyOnce)
{
    std::string const first = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(first, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tail and the consumed-record counts below are a coin
    // flip. pollManifestTail() does not consult the shutdown flag.
    agent.shutdown();

    std::string const second = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifest(second, BASE + 20 * NS, BASE + 30 * NS);

    EXPECT_EQ(agent.pollManifestTail(), 1) << "expected exactly the one new record";
    EXPECT_EQ(agent.pollManifestTail(), 0) << "a second poll re-read records it had already consumed";
}

TEST_F(ArchiveIndexTest, APartiallyWrittenRecordIsNotConsumedUntilComplete)
{
    std::string const first = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(first, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tail and the consumed-record counts below are a coin
    // flip. pollManifestTail() does not consult the shutdown flag.
    agent.shutdown();

    // The Grapher is mid-append: bytes are there, the newline is not. Consuming
    // them would skip the record permanently once the rest arrived.
    std::string const second = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    std::string const line = chl::toManifestLine(
            [&]
            {
                chl::ArchiveManifestRecord r;
                r.chronicle = "c1";
                r.story = "s1";
                r.story_id = STORY_ID;
                r.file = second;
                r.start = BASE + 20 * NS;
                r.end = BASE + 30 * NS;
                r.state = chl::ManifestState::PUBLISHED;
                return r;
            }());
    {
        std::ofstream log(root / "archive_manifest.log", std::ios::app);
        log << line.substr(0, line.size() / 2); // no trailing newline
    }

    EXPECT_EQ(agent.pollManifestTail(), 0) << "a torn line was consumed";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 0u);

    // The rest of the record lands.
    {
        std::ofstream log(root / "archive_manifest.log", std::ios::app);
        log << line.substr(line.size() / 2) << "\n";
    }

    EXPECT_EQ(agent.pollManifestTail(), 1) << "the completed record was never picked up";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u);

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, ADeletionAppendedAfterStartupRemovesTheFile)
{
    std::string const file = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(file, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tail and the consumed-record counts below are a coin
    // flip. pollManifestTail() does not consult the shutdown flag.
    agent.shutdown();
    ASSERT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u);

    {
        chl::ArchiveManifest manifest(root.string());
        chl::ArchiveManifestRecord removal;
        removal.chronicle = "c1";
        removal.story = "s1";
        removal.story_id = STORY_ID;
        removal.file = file;
        removal.state = chl::ManifestState::DELETED;
        manifest.append(removal);
    }

    EXPECT_GT(agent.pollManifestTail(), 0);
    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 0u) << "a deleted file is still being read";

    agent.shutdown();
}

TEST_F(ArchiveIndexTest, CompactionTruncatingTheLogDoesNotLoseTheIndex)
{
    // Compaction folds the log into the snapshot and truncates it, so the log
    // SHRINKS. A poller that only ever advanced its offset would then read past
    // the end, or treat the new short log as unread tail and re-apply nothing.
    std::string const first = writeChunk(BASE, BASE + 10 * NS);
    recordInManifest(first, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tail and the consumed-record counts below are a coin
    // flip. pollManifestTail() does not consult the shutdown flag.
    agent.shutdown();
    ASSERT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u);

    {
        chl::ArchiveManifest manifest(root.string());
        ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.snapshot(), chl::CL_SUCCESS); // truncates the log
    }
    std::string const second = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifest(second, BASE + 20 * NS, BASE + 30 * NS);

    EXPECT_GE(agent.pollManifestTail(), 1) << "records after a compaction were not picked up";
    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u) << "the pre-compaction file was lost from the index";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u);

    agent.shutdown();
}

// ---------------------------------------------------------------------------
// Several Graphers, several manifest logs.
//
// Each Grapher writes its own log because sharing one over NFSv3 loses records
// (manifest_plan/nfs_validation.md). That only works if the Player treats the set
// of logs as the manifest, both at startup and while polling.
// ---------------------------------------------------------------------------

TEST_F(ArchiveIndexTest, IndexIsBuiltFromEveryGraphersManifest)
{
    std::string const from_first = writeChunk(BASE, BASE + 10 * NS);
    std::string const from_second = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    recordInManifestFor("1", from_first, BASE, BASE + 10 * NS);
    recordInManifestFor("2", from_second, BASE + 20 * NS, BASE + 30 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    agent.shutdown();

    EXPECT_EQ(eventsInRange(agent, BASE, BASE + 10 * NS), 3u) << "the first Grapher's window is missing";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u) << "the second Grapher's window is missing";
}

TEST_F(ArchiveIndexTest, PollingFollowsEveryGraphersLog)
{
    std::string const seed = writeChunk(BASE, BASE + 10 * NS);
    recordInManifestFor("1", seed, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    // Stop the monitoring thread before stepping the poll by hand: otherwise it
    // races for the same tails and the consumed-record counts below are a coin flip.
    agent.shutdown();

    std::string const from_first = writeChunk(BASE + 20 * NS, BASE + 30 * NS);
    std::string const from_second = writeChunk(BASE + 40 * NS, BASE + 50 * NS);
    recordInManifestFor("1", from_first, BASE + 20 * NS, BASE + 30 * NS);
    recordInManifestFor("2", from_second, BASE + 40 * NS, BASE + 50 * NS);

    EXPECT_EQ(agent.pollManifestTail(), 2) << "one poll must drain every Grapher's tail";
    EXPECT_EQ(eventsInRange(agent, BASE + 20 * NS, BASE + 30 * NS), 3u);
    EXPECT_EQ(eventsInRange(agent, BASE + 40 * NS, BASE + 50 * NS), 3u);
    // Offsets are per log: one shared offset would make the shorter log look
    // already-consumed and silently skip its records.
    EXPECT_EQ(agent.pollManifestTail(), 0) << "a second poll re-read records it had already consumed";
}

TEST_F(ArchiveIndexTest, AGrapherThatStartsAfterThePlayerIsPickedUp)
{
    std::string const seed = writeChunk(BASE, BASE + 10 * NS);
    recordInManifestFor("1", seed, BASE, BASE + 10 * NS);

    chl::HDF5ArchiveReadingAgent agent(root.string(), true, std::chrono::milliseconds(50));
    ASSERT_EQ(agent.initialize(), 0);
    agent.shutdown();

    // A log that did not exist when the Player started. Discovering logs once at
    // startup would leave this Grapher's output invisible for the Player's lifetime.
    std::string const latecomer = writeChunk(BASE + 60 * NS, BASE + 70 * NS);
    recordInManifestFor("9", latecomer, BASE + 60 * NS, BASE + 70 * NS);

    EXPECT_EQ(agent.pollManifestTail(), 1) << "a manifest log created after startup was never noticed";
    EXPECT_EQ(eventsInRange(agent, BASE + 60 * NS, BASE + 70 * NS), 3u);
}
