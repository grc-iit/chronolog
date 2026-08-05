// HDF5FileChunkExtractor must record every outcome in the archive manifest, not
// just the successful ones, because the manifest is what a restarted grapher
// replays to recover each story's persisted watermark W.
//
// The three outcomes process_chunk already distinguishes have to stay
// distinguishable in the manifest, and the distinction is not cosmetic:
//
//   published  a real file exists; the window counts toward W.
//   empty      no events, so no file, but the window WAS vacuously persisted and
//              must count -- omitting it leaves a hole that truncates the
//              contiguous prefix and makes restored W regress.
//   failed     the write failed, so the window was NOT persisted. Recording it as
//              "empty" would advance W over events that were never written, which
//              is the one direction E <= W cannot absorb.
//
// Deletions are recorded too: the log is append-only, so removing a story's files
// appends records that supersede their publications.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <thallium.hpp>

#include <chrono_monitor.h>

#include <ArchiveManifest.h>
#include <HDF5FileChunkExtractor.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t NS = 1000000000ULL;
constexpr uint64_t CHUNK_START = 1736800000ULL * NS;
constexpr uint64_t CHUNK_END = CHUNK_START + 10 * NS;
constexpr chl::StoryId STORY_ID = 4242;

class GrapherArchiveManifestTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() / ("chronolog_grapher_manifest_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    chl::StoryChunk makeChunk(int events, uint64_t start = CHUNK_START, uint64_t end = CHUNK_END)
    {
        chl::StoryChunk chunk("c1", "cpu_usage", STORY_ID, start, end);
        for(int i = 0; i < events; i++)
        {
            chunk.insertEvent(chl::LogEvent(STORY_ID, start + (uint64_t)(i + 1), 7, (chl::chrono_index)i, "payload"));
        }
        return chunk;
    }

    std::vector<chl::ArchiveManifestRecord> reloadRecords()
    {
        chl::ArchiveManifest reloaded(root.string());
        EXPECT_EQ(reloaded.load(), chl::CL_SUCCESS);
        return reloaded.records();
    }

    fs::path root;
};

} // namespace


namespace
{

// Runs reconciliation through an extractor pointed at the same directory, which is
// how the Grapher does it at startup.
int reconcile(chl::ArchiveManifest& manifest, fs::path const& root)
{
    chl::HDF5FileChunkExtractor extractor;
    EXPECT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);
    extractor.attachArchiveManifest(&manifest);
    return extractor.reconcileManifestWithDirectory();
}

chl::ArchiveManifestRecord publishedRecord(chl::StoryId story_id, uint64_t start, uint64_t end)
{
    chl::ArchiveManifestRecord record;
    record.chronicle = "c1";
    record.story = "cpu_usage";
    record.story_id = story_id;
    record.start = start;
    record.end = end;
    record.state = chl::ManifestState::PUBLISHED;
    return record;
}

} // namespace

TEST_F(GrapherArchiveManifestTest, APublishedChunkIsRecordedWithItsFileAndWindow)
{
    chl::ArchiveManifest manifest(root.string());
    chl::HDF5FileChunkExtractor extractor;
    ASSERT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);
    extractor.attachArchiveManifest(&manifest);

    chl::StoryChunk chunk = makeChunk(3);
    ASSERT_EQ(extractor.process_chunk(&chunk), chl::CL_SUCCESS);

    std::vector<chl::ArchiveManifestRecord> const records = reloadRecords();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].state, chl::ManifestState::PUBLISHED);
    EXPECT_EQ(records[0].chronicle, "c1");
    EXPECT_EQ(records[0].story, "cpu_usage");
    EXPECT_EQ(records[0].story_id, STORY_ID);
    EXPECT_EQ(records[0].start, CHUNK_START);
    EXPECT_EQ(records[0].end, CHUNK_END);
    EXPECT_EQ(records[0].events, 3u);
    EXPECT_FALSE(records[0].exempt);
    // The recorded name must be the file that is actually on disk, or a restarted
    // reader indexes something that is not there.
    EXPECT_FALSE(records[0].file.empty());
    EXPECT_TRUE(fs::exists(root / fs::path(records[0].file).filename())) << "recorded file: " << records[0].file;
}

TEST_F(GrapherArchiveManifestTest, AnEmptyWindowIsRecordedWithNoFileAndStillCountsTowardW)
{
    chl::ArchiveManifest manifest(root.string());
    chl::HDF5FileChunkExtractor extractor;
    ASSERT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);
    extractor.attachArchiveManifest(&manifest);

    chl::StoryChunk first = makeChunk(2, CHUNK_START, CHUNK_START + 10 * NS);
    chl::StoryChunk gap = makeChunk(0, CHUNK_START + 10 * NS, CHUNK_START + 20 * NS);
    chl::StoryChunk third = makeChunk(2, CHUNK_START + 20 * NS, CHUNK_START + 30 * NS);
    ASSERT_EQ(extractor.process_chunk(&first), chl::CL_SUCCESS);
    ASSERT_EQ(extractor.process_chunk(&gap), chl::CL_SUCCESS);
    ASSERT_EQ(extractor.process_chunk(&third), chl::CL_SUCCESS);

    std::vector<chl::ArchiveManifestRecord> const records = reloadRecords();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[1].state, chl::ManifestState::EMPTY);
    EXPECT_TRUE(records[1].file.empty()) << "an empty window writes no file";

    chl::ArchiveManifest reloaded(root.string());
    ASSERT_EQ(reloaded.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reloaded.deriveWatermarks().at(STORY_ID), CHUNK_START + 30 * NS)
            << "the empty window must keep the contiguous run intact";
}

TEST_F(GrapherArchiveManifestTest, ASalvageChunkIsRecordedAsExemptAndDoesNotAdvanceW)
{
    chl::ArchiveManifest manifest(root.string());
    chl::HDF5FileChunkExtractor extractor;
    ASSERT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);
    extractor.attachArchiveManifest(&manifest);

    chl::StoryChunk salvage = makeChunk(2);
    salvage.setWatermarkExempt(true);
    ASSERT_EQ(extractor.process_chunk(&salvage), chl::CL_SUCCESS);

    std::vector<chl::ArchiveManifestRecord> const records = reloadRecords();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_TRUE(records[0].exempt) << "a salvage chunk is a real file but not a merged timeline window";

    chl::ArchiveManifest reloaded(root.string());
    ASSERT_EQ(reloaded.load(), chl::CL_SUCCESS);
    EXPECT_TRUE(reloaded.deriveWatermarks().empty());
}

TEST_F(GrapherArchiveManifestTest, DeletingAStorysFilesAppendsRecordsThatSupersedeThem)
{
    chl::ArchiveManifest manifest(root.string());
    chl::HDF5FileChunkExtractor extractor;
    ASSERT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);
    extractor.attachArchiveManifest(&manifest);

    chl::StoryChunk first = makeChunk(2, CHUNK_START, CHUNK_START + 10 * NS);
    chl::StoryChunk second = makeChunk(2, CHUNK_START + 10 * NS, CHUNK_START + 20 * NS);
    ASSERT_EQ(extractor.process_chunk(&first), chl::CL_SUCCESS);
    ASSERT_EQ(extractor.process_chunk(&second), chl::CL_SUCCESS);
    {
        chl::ArchiveManifest before(root.string());
        ASSERT_EQ(before.load(), chl::CL_SUCCESS);
        ASSERT_EQ(before.deriveWatermarks().at(STORY_ID), CHUNK_START + 20 * NS);
    }

    size_t deleted = 0;
    ASSERT_EQ(extractor.delete_story_files("c1", "cpu_usage", &deleted), chl::CL_SUCCESS);
    EXPECT_EQ(deleted, 2u);

    chl::ArchiveManifest after(root.string());
    ASSERT_EQ(after.load(), chl::CL_SUCCESS);
    std::vector<chl::ArchiveManifestRecord> const records = after.records();
    size_t deleted_records = 0;
    for(auto const& record: records)
    {
        if(record.state == chl::ManifestState::DELETED)
        {
            deleted_records++;
        }
    }
    EXPECT_EQ(deleted_records, 2u) << "each removed file needs its own superseding record";
    EXPECT_TRUE(after.deriveWatermarks().empty()) << "no file survives, so nothing is persisted";
}

TEST_F(GrapherArchiveManifestTest, WorksWithNoManifestAttached)
{
    // The manifest is optional: an extractor with none attached must behave
    // exactly as before rather than crashing on a null pointer.
    chl::HDF5FileChunkExtractor extractor;
    ASSERT_EQ(extractor.reset(root.string() + "/"), chl::CL_SUCCESS);

    chl::StoryChunk chunk = makeChunk(2);
    EXPECT_EQ(extractor.process_chunk(&chunk), chl::CL_SUCCESS);

    size_t deleted = 0;
    EXPECT_EQ(extractor.delete_story_files("c1", "cpu_usage", &deleted), chl::CL_SUCCESS);
    EXPECT_EQ(deleted, 1u);
}


// ---- adopting files the manifest does not know about -----------------------
//
// A record is appended only AFTER its file is published, so a crash in between
// leaves an HDF5 file on disk that the manifest never mentions. Nothing repairs
// that on its own: the file stays unreferenced, invisible to every reader, and its
// window keeps W held back forever.
//
// Reconciliation is the Grapher's job rather than each reader's, because the
// manifest is the Grapher's artifact. Repairing it once fixes every reader AND
// restores W, whereas a reader-side directory scan papers over the gap for that
// reader only and leaves the manifest permanently wrong.
//
// What an adopted record may claim is bounded by what the file itself proves. The
// name gives chronicle, story and window start; the events inside give the story id
// and the last event time. The window's true END is unknowable -- it may have
// extended past the last event -- so `end` is taken as last_event + 1. That is
// always <= the truth, which under-reports W and skips only files whose events are
// genuinely all below a query, both of which are the safe direction.

namespace
{

// Writes a real archive file the way the Grapher would, and returns its base name.
std::string writeArchiveFile(fs::path const& root, chl::StoryId story_id, uint64_t start, uint64_t end, int events)
{
    chl::StoryChunk chunk("c1", "cpu_usage", story_id, start, end);
    for(int i = 0; i < events; i++)
    {
        chunk.insertEvent(chl::LogEvent(story_id, start + (uint64_t)(i + 1), 7, (chl::chrono_index)i, "payload"));
    }
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunkWriteResult const result = writer.writeStoryChunk(chunk);
    EXPECT_GT(result.file_size, 0u);
    return fs::path(result.file_name).filename().string();
}

} // namespace

TEST_F(GrapherArchiveManifestTest, AnUnrecordedFileIsAdopted)
{
    // The crash case: the file was published, the append never happened.
    std::string const orphan = writeArchiveFile(root, 55, 100 * NS, 110 * NS, 3);

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
    ASSERT_TRUE(manifest.records().empty());

    EXPECT_EQ(reconcile(manifest, root), 1);

    std::vector<chl::ArchiveManifestRecord> const records = manifest.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, orphan);
    EXPECT_EQ(records[0].state, chl::ManifestState::PUBLISHED);
    EXPECT_EQ(records[0].story_id, 55u) << "the story id has to come from the file, not from hashing its name";
    EXPECT_EQ(records[0].start, 100 * NS);
    EXPECT_GT(records[0].end, records[0].start);
    EXPECT_LE(records[0].end, 110 * NS) << "end must not claim more than the events prove";

    // and it survives a reload, i.e. it really was appended
    chl::ArchiveManifest reloaded(root.string());
    ASSERT_EQ(reloaded.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reloaded.records().size(), 1u);
}

TEST_F(GrapherArchiveManifestTest, AdoptionRestoresTheWatermarkForTheOrphanedWindow)
{
    std::string const orphan = writeArchiveFile(root, 55, 100 * NS, 110 * NS, 3);

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
    EXPECT_TRUE(manifest.deriveWatermarks().empty()) << "nothing is known before reconciliation";

    ASSERT_EQ(reconcile(manifest, root), 1);

    std::map<chl::StoryId, uint64_t> const w = manifest.deriveWatermarks();
    ASSERT_EQ(w.count(55), 1u) << "the whole point of repairing the manifest is that W comes back";
    EXPECT_GT(w.at(55), 100 * NS);
}

TEST_F(GrapherArchiveManifestTest, AlreadyRecordedFilesAreNotAdoptedAgain)
{
    std::string const known = writeArchiveFile(root, 55, 100 * NS, 110 * NS, 3);

    chl::ArchiveManifest manifest(root.string());
    chl::ArchiveManifestRecord record = publishedRecord(55, 100 * NS, 110 * NS);
    record.file = known;
    ASSERT_EQ(manifest.append(record), chl::CL_SUCCESS);

    EXPECT_EQ(reconcile(manifest, root), 0);
    EXPECT_EQ(manifest.records().size(), 1u);
}

TEST_F(GrapherArchiveManifestTest, ReconciliationIsIdempotent)
{
    // Every Grapher sharing the archive directory reconciles at startup, so this
    // runs repeatedly and concurrently over the same files.
    writeArchiveFile(root, 55, 100 * NS, 110 * NS, 3);

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
    ASSERT_EQ(reconcile(manifest, root), 1);
    EXPECT_EQ(reconcile(manifest, root), 0) << "a second pass adopted the same file twice";
    EXPECT_EQ(manifest.records().size(), 1u);
}

TEST_F(GrapherArchiveManifestTest, ADeletedFileIsNotResurrectedByAdoption)
{
    // If a delete removed the record but the unlink did not finish, the bytes are
    // still there. Adopting them would undo the deletion.
    std::string const removed = writeArchiveFile(root, 55, 100 * NS, 110 * NS, 3);

    chl::ArchiveManifest manifest(root.string());
    chl::ArchiveManifestRecord record = publishedRecord(55, 100 * NS, 110 * NS);
    record.file = removed;
    ASSERT_EQ(manifest.append(record), chl::CL_SUCCESS);
    chl::ArchiveManifestRecord removal = record;
    removal.state = chl::ManifestState::DELETED;
    ASSERT_EQ(manifest.append(removal), chl::CL_SUCCESS);

    EXPECT_EQ(reconcile(manifest, root), 0) << "a deleted file was adopted back into the manifest";
    EXPECT_TRUE(manifest.deriveWatermarks().empty());
}

TEST_F(GrapherArchiveManifestTest, NonArchiveFilesAreIgnored)
{
    {
        std::ofstream stray(root / "notes.txt");
        stray << "not an archive file";
    }
    {
        std::ofstream partial(root / "c1.cpu_usage.100.vlen.h5.1234.0.tmp");
        partial << "an in-progress write";
    }

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reconcile(manifest, root), 0) << "reconciliation must not index non-archive files";
}

// process_chunk logs thallium::thread::self_id(), so Argobots has to be running
// before any test body executes.
int main(int argc, char** argv)
{
    thallium::abt scope;
    chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "grapher_manifest_test_logger");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
