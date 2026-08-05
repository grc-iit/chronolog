// ArchiveManifest: the durable record of what the grapher has published to the
// archive, and the source from which a restarted grapher restores each story's
// persisted watermark W.
//
// Two properties carry the whole design and are what these tests pin down:
//
//   - Losing trailing appends must only ever make W *under*-report. Under E <= W
//     that is safe (keepers retain longer and re-send); over-reporting would let a
//     keeper drop events that were never actually persisted. So a torn final line
//     is discarded rather than guessed at, and anything not positively recorded as
//     persisted is excluded.
//   - Restored W must equal pre-restart W exactly, which is why an empty chunk
//     window is recorded at all: it writes no HDF5 file but still counts as a
//     persisted interval, and omitting it would leave a hole that truncates the
//     contiguous run.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#include <ArchiveManifest.h>
#include <ConfigurationBlocks.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t NS = 1000000000ULL;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "archive_manifest_test_logger");
        done = true;
    }
}

chl::ArchiveManifestRecord published(chl::StoryId story_id, uint64_t start, uint64_t end, uint32_t seq = 0)
{
    chl::ArchiveManifestRecord rec;
    rec.chronicle = "c1";
    rec.story = "cpu_usage";
    rec.story_id = story_id;
    rec.file = "c1.cpu_usage." + std::to_string(start / NS) + ".vlen.h5";
    rec.start = start;
    rec.end = end;
    rec.seq = seq;
    rec.events = 42;
    rec.state = chl::ManifestState::PUBLISHED;
    rec.exempt = false;
    return rec;
}

class ArchiveManifestTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        root = fs::temp_directory_path() / ("chronolog_manifest_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;
};

} // namespace

// ---- record serialization --------------------------------------------------

TEST_F(ArchiveManifestTest, RecordRoundTripsThroughAJsonLine)
{
    chl::ArchiveManifestRecord original = published(77, 100 * NS, 160 * NS, 3);
    original.exempt = true;
    original.state = chl::ManifestState::DELETED;

    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(original), parsed));

    EXPECT_EQ(parsed.version, original.version);
    EXPECT_EQ(parsed.chronicle, original.chronicle);
    EXPECT_EQ(parsed.story, original.story);
    EXPECT_EQ(parsed.story_id, original.story_id);
    EXPECT_EQ(parsed.file, original.file);
    EXPECT_EQ(parsed.start, original.start);
    EXPECT_EQ(parsed.end, original.end);
    EXPECT_EQ(parsed.seq, original.seq);
    EXPECT_EQ(parsed.events, original.events);
    EXPECT_EQ(parsed.state, original.state);
    EXPECT_EQ(parsed.exempt, original.exempt);
}

TEST_F(ArchiveManifestTest, ASerializedRecordIsExactlyOneLine)
{
    // The log is newline-delimited; an embedded newline would corrupt every
    // record after it.
    std::string const line = chl::toManifestLine(published(77, 100 * NS, 160 * NS));
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST_F(ArchiveManifestTest, MalformedLinesAreRejectedRatherThanPartiallyAccepted)
{
    chl::ArchiveManifestRecord parsed;
    EXPECT_FALSE(chl::parseManifestLine("", parsed));
    EXPECT_FALSE(chl::parseManifestLine("{\"v\":1,\"chronicle\":\"c1\"", parsed)) << "truncated JSON";
    EXPECT_FALSE(chl::parseManifestLine("not json at all", parsed));
    EXPECT_FALSE(chl::parseManifestLine("[1,2,3]", parsed)) << "JSON, but not an object";
}

// ---- append / load ---------------------------------------------------------

TEST_F(ArchiveManifestTest, AppendedRecordsLoadBackInOrder)
{
    {
        chl::ArchiveManifest manifest(root.string());
        ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.append(published(2, 50 * NS, 60 * NS)), chl::CL_SUCCESS);
    }

    chl::ArchiveManifest reopened(root.string());
    ASSERT_EQ(reopened.load(), chl::CL_SUCCESS);
    ASSERT_EQ(reopened.records().size(), 3u);
    EXPECT_EQ(reopened.records()[0].start, 0 * NS);
    EXPECT_EQ(reopened.records()[1].start, 10 * NS);
    EXPECT_EQ(reopened.records()[2].story_id, 2u);
}

TEST_F(ArchiveManifestTest, LoadingAnAbsentManifestIsNotAnError)
{
    // A fresh archive root has no manifest yet; that is the normal first-start
    // path, not a failure.
    chl::ArchiveManifest manifest(root.string());
    EXPECT_EQ(manifest.load(), chl::CL_SUCCESS);
    EXPECT_TRUE(manifest.records().empty());
}

TEST_F(ArchiveManifestTest, ATornFinalLineIsDiscardedAndEarlierRecordsSurvive)
{
    {
        chl::ArchiveManifest manifest(root.string());
        ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
    }

    // Simulate a crash partway through an append.
    {
        std::ofstream log(root / "archive_manifest.log", std::ios::app);
        log << "{\"v\":1,\"chronicle\":\"c1\",\"story\":\"cpu_usage\",\"start\":200000";
    }

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.load(), chl::CL_SUCCESS);
    EXPECT_EQ(manifest.records().size(), 2u) << "a torn append must not take the whole manifest down";
    EXPECT_EQ(manifest.records().back().end, 20 * NS);
}

// ---- snapshot compaction ---------------------------------------------------

TEST_F(ArchiveManifestTest, SnapshotPreservesEveryRecordAndTruncatesTheLog)
{
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.snapshot(), chl::CL_SUCCESS);

    EXPECT_TRUE(fs::exists(root / "archive_manifest.json"));
    EXPECT_EQ(fs::file_size(root / "archive_manifest.log"), 0u) << "log should be truncated after compaction";
    EXPECT_FALSE(fs::exists(root / "archive_manifest.json.tmp")) << "the temp file must be renamed, not left behind";

    chl::ArchiveManifest reopened(root.string());
    ASSERT_EQ(reopened.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reopened.records().size(), 2u);
}

TEST_F(ArchiveManifestTest, RecordsAppendedAfterASnapshotAreLoadedOnTopOfIt)
{
    {
        chl::ArchiveManifest manifest(root.string());
        ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.snapshot(), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
    }

    chl::ArchiveManifest reopened(root.string());
    ASSERT_EQ(reopened.load(), chl::CL_SUCCESS);
    ASSERT_EQ(reopened.records().size(), 2u) << "snapshot and post-snapshot log must both be read";
    EXPECT_EQ(reopened.deriveWatermarks().at(1), 20 * NS);
}

// ---- watermark derivation --------------------------------------------------

TEST_F(ArchiveManifestTest, WatermarkAdvancesOverAContiguousRun)
{
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 20 * NS, 30 * NS)), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 30 * NS);
}

TEST_F(ArchiveManifestTest, WatermarkStopsAtTheFirstGap)
{
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    // 10..20 never published -- W must not jump the hole.
    ASSERT_EQ(manifest.append(published(1, 20 * NS, 30 * NS)), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS);
}

TEST_F(ArchiveManifestTest, OutOfOrderRecordsStillYieldTheSameWatermark)
{
    // Records land in completion order, not window order.
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 20 * NS, 30 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 30 * NS);
}

TEST_F(ArchiveManifestTest, AnEmptyWindowKeepsTheContiguousRunIntact)
{
    // An empty chunk writes no file but was still "persisted" vacuously. Without
    // its record the run breaks at 10s and restored W would regress.
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);

    chl::ArchiveManifestRecord empty = published(1, 10 * NS, 20 * NS);
    empty.state = chl::ManifestState::EMPTY;
    empty.file.clear();
    empty.events = 0;
    ASSERT_EQ(manifest.append(empty), chl::CL_SUCCESS);

    ASSERT_EQ(manifest.append(published(1, 20 * NS, 30 * NS)), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 30 * NS);
}

TEST_F(ArchiveManifestTest, ExemptRecordsDoNotAdvanceTheWatermark)
{
    // A salvage chunk is a real file the reader must see, but its events were
    // never merged into the contiguous timeline, so it must not advance W.
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);

    chl::ArchiveManifestRecord salvage = published(1, 10 * NS, 20 * NS, 1);
    salvage.exempt = true;
    ASSERT_EQ(manifest.append(salvage), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS);
}

TEST_F(ArchiveManifestTest, DeletedRecordsDoNotAdvanceTheWatermark)
{
    // The file is gone; claiming its window as persisted would over-report W,
    // the one direction the E <= W invariant cannot absorb.
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);

    chl::ArchiveManifestRecord deleted = published(1, 10 * NS, 20 * NS);
    deleted.state = chl::ManifestState::DELETED;
    ASSERT_EQ(manifest.append(deleted), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS);
}

TEST_F(ArchiveManifestTest, WatermarksAreDerivedPerStoryIndependently)
{
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(2, 100 * NS, 130 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);

    std::map<chl::StoryId, uint64_t> const w = manifest.deriveWatermarks();
    EXPECT_EQ(w.at(1), 20 * NS);
    EXPECT_EQ(w.at(2), 130 * NS) << "a story's W is anchored at its own first window, not at zero";
    EXPECT_EQ(w.size(), 2u);
}

TEST_F(ArchiveManifestTest, AStoryIdAboveInt64MaxSurvivesTheRoundTrip)
{
    // StoryIds are CityHash64 outputs and routinely exceed INT64_MAX (this one is
    // taken from a real deployment log). JSON has no unsigned integer type, so a
    // naive int64 encoding writes a negative number; the manifest must still read
    // back the exact id, or W is restored onto a story that does not exist.
    constexpr chl::StoryId BIG_STORY_ID = 10572220622238703070ULL;
    ASSERT_GT(BIG_STORY_ID, static_cast<chl::StoryId>(INT64_MAX));

    chl::ArchiveManifestRecord original = published(BIG_STORY_ID, 100 * NS, 160 * NS);

    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(original), parsed));
    EXPECT_EQ(parsed.story_id, BIG_STORY_ID);
}

TEST_F(ArchiveManifestTest, WatermarksKeyOffLargeStoryIdsCorrectly)
{
    constexpr chl::StoryId BIG_STORY_ID = 10572220622238703070ULL;

    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(BIG_STORY_ID, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(published(BIG_STORY_ID, 10 * NS, 20 * NS)), chl::CL_SUCCESS);

    std::map<chl::StoryId, uint64_t> const w = manifest.deriveWatermarks();
    ASSERT_EQ(w.count(BIG_STORY_ID), 1u) << "large StoryId did not survive to the watermark map";
    EXPECT_EQ(w.at(BIG_STORY_ID), 20 * NS);
}

TEST_F(ArchiveManifestTest, TheOnDiskStoryIdIsReadableByExternalTooling)
{
    // JSON has no unsigned 64-bit type, so encoding a StoryId as a JSON integer
    // writes it as a negative number once it passes INT64_MAX. It round-trips
    // through this code, but the manifest is meant to be inspectable (jq, ops
    // scripts, bug reports), and no external reader will match a story by its
    // two's-complement alias. Store it as a decimal string instead.
    constexpr chl::StoryId BIG_STORY_ID = 10572220622238703070ULL;
    std::string const line = chl::toManifestLine(published(BIG_STORY_ID, 100 * NS, 160 * NS));

    EXPECT_EQ(line.find('-'), std::string::npos) << "no field should be encoded as a negative number: " << line;
    EXPECT_NE(line.find("10572220622238703070"), std::string::npos)
            << "the story id should appear verbatim in the line: " << line;
}

TEST_F(ArchiveManifestTest, AStoryIdWrittenAsAJsonIntegerIsStillAccepted)
{
    // Tolerated so a hand-written or older manifest still loads.
    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine("{\"v\":1,\"chronicle\":\"c1\",\"story\":\"s\",\"story_id\":77,"
                                       "\"file\":\"f.h5\",\"start\":0,\"end\":10,\"seq\":0,\"events\":1,"
                                       "\"state\":\"published\",\"exempt\":false}",
                                       parsed));
    EXPECT_EQ(parsed.story_id, 77u);
}

// ---- records that supersede earlier ones -----------------------------------

TEST_F(ArchiveManifestTest, ADeletedFileStopsCountingAsPublished)
{
    // The log is append-only, so a deletion arrives as a later record rather than
    // by removing the earlier one. Skipping only the deleted record would leave
    // the original publication still counted -- the file would be gone from disk
    // and still claimed as persisted, which is the over-reporting direction.
    chl::ArchiveManifest manifest(root.string());
    chl::ArchiveManifestRecord first = published(1, 0 * NS, 10 * NS);
    chl::ArchiveManifestRecord second = published(1, 10 * NS, 20 * NS);
    ASSERT_EQ(manifest.append(first), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(second), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.deriveWatermarks().at(1), 20 * NS);

    chl::ArchiveManifestRecord removal = second;
    removal.state = chl::ManifestState::DELETED;
    ASSERT_EQ(manifest.append(removal), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS) << "a deleted file must stop contributing to W";
}

TEST_F(ArchiveManifestTest, DeletingOneRotationLeavesTheOthersAlone)
{
    // Deletion is matched on the file name, so removing a rotated file must not
    // take its base sibling with it.
    chl::ArchiveManifest manifest(root.string());
    chl::ArchiveManifestRecord base = published(1, 0 * NS, 10 * NS, 0);
    chl::ArchiveManifestRecord rotated = published(1, 0 * NS, 10 * NS, 1);
    rotated.file = "c1.cpu_usage.0.vlen.1.h5";
    ASSERT_EQ(manifest.append(base), chl::CL_SUCCESS);
    ASSERT_EQ(manifest.append(rotated), chl::CL_SUCCESS);

    chl::ArchiveManifestRecord removal = rotated;
    removal.state = chl::ManifestState::DELETED;
    ASSERT_EQ(manifest.append(removal), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS) << "the surviving base file still covers the window";
}

TEST_F(ArchiveManifestTest, AFailedWriteIsRecordedWithoutAdvancingTheWatermark)
{
    // A failed HDF5 write produced no file, so its window is not persisted. It is
    // still worth recording (an operator wants to see it), but recording it as
    // "empty" would advance W over a window whose events were never written.
    chl::ArchiveManifest manifest(root.string());
    ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);

    chl::ArchiveManifestRecord failed = published(1, 10 * NS, 20 * NS);
    failed.state = chl::ManifestState::FAILED;
    failed.file.clear();
    ASSERT_EQ(manifest.append(failed), chl::CL_SUCCESS);

    EXPECT_EQ(manifest.deriveWatermarks().at(1), 10 * NS);
    EXPECT_EQ(manifest.records().size(), 2u) << "the failure should still be visible in the manifest";
}

TEST_F(ArchiveManifestTest, FailedRoundTripsThroughAJsonLine)
{
    chl::ArchiveManifestRecord original = published(1, 0 * NS, 10 * NS);
    original.state = chl::ManifestState::FAILED;

    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(original), parsed));
    EXPECT_EQ(parsed.state, chl::ManifestState::FAILED);
}

// ---- concurrency -----------------------------------------------------------

TEST_F(ArchiveManifestTest, ConcurrentAppendsAllSurvive)
{
    // The grapher's extraction module runs several streams, and they all publish
    // into one manifest, so append is called concurrently. Without serialization
    // the interleaved writes tear each other's lines and the in-memory vector
    // races.
    constexpr int THREADS = 8;
    constexpr int PER_THREAD = 50;

    {
        chl::ArchiveManifest manifest(root.string());
        std::vector<std::thread> writers;
        for(int t = 0; t < THREADS; ++t)
        {
            writers.emplace_back(
                    [&manifest, t]()
                    {
                        for(int i = 0; i < PER_THREAD; ++i)
                        {
                            manifest.append(published(static_cast<chl::StoryId>(t),
                                                      static_cast<uint64_t>(i) * NS,
                                                      static_cast<uint64_t>(i + 1) * NS));
                        }
                    });
        }
        for(auto& writer: writers) { writer.join(); }
        EXPECT_EQ(manifest.records().size(), static_cast<size_t>(THREADS * PER_THREAD));
    }

    chl::ArchiveManifest reopened(root.string());
    ASSERT_EQ(reopened.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reopened.records().size(), static_cast<size_t>(THREADS * PER_THREAD))
            << "interleaved appends tore each other's lines";

    std::map<chl::StoryId, uint64_t> const w = reopened.deriveWatermarks();
    EXPECT_EQ(w.size(), static_cast<size_t>(THREADS));
    for(int t = 0; t < THREADS; ++t)
    {
        EXPECT_EQ(w.at(static_cast<chl::StoryId>(t)), static_cast<uint64_t>(PER_THREAD) * NS);
    }
}

// ---- configuration ---------------------------------------------------------

TEST_F(ArchiveManifestTest, ManifestKnobsParseAndDefault)
{
    // The grapher-side knobs live in DataStoreInternals. Defaults matter as much
    // as parsing: manifest_enabled defaults on (a Grapher that silently stopped
    // recording would only be discovered at the next restart, when W came back 0),
    // and manifest_fsync defaults off (a record is appended only after its file is
    // published, so losing trailing appends is the safe direction).
    chl::DataStoreConf defaults;
    EXPECT_TRUE(defaults.manifest_enabled);
    EXPECT_FALSE(defaults.manifest_fsync);
    EXPECT_EQ(defaults.manifest_snapshot_threshold_entries, 10000);

    chl::DataStoreConf parsed;
    json_object* conf = json_tokener_parse("{\"manifest_enabled\":false,"
                                           "\"manifest_snapshot_threshold_entries\":42,"
                                           "\"manifest_fsync\":true}");
    ASSERT_NE(conf, nullptr);
    ASSERT_EQ(parsed.parseJsonConf(conf), chl::CL_SUCCESS);
    json_object_put(conf);

    EXPECT_FALSE(parsed.manifest_enabled);
    EXPECT_EQ(parsed.manifest_snapshot_threshold_entries, 42);
    EXPECT_TRUE(parsed.manifest_fsync);
}

TEST_F(ArchiveManifestTest, ManifestKnobsRejectTheWrongJsonType)
{
    chl::DataStoreConf conf_block;
    json_object* bad = json_tokener_parse("{\"manifest_enabled\":\"yes\"}");
    ASSERT_NE(bad, nullptr);
    EXPECT_EQ(conf_block.parseJsonConf(bad), chl::CL_ERR_INVALID_CONF);
    json_object_put(bad);

    json_object* bad_threshold = json_tokener_parse("{\"manifest_snapshot_threshold_entries\":\"lots\"}");
    ASSERT_NE(bad_threshold, nullptr);
    EXPECT_EQ(conf_block.parseJsonConf(bad_threshold), chl::CL_ERR_INVALID_CONF);
    json_object_put(bad_threshold);
}

TEST_F(ArchiveManifestTest, FsyncingAppendsStillProducesAReadableManifest)
{
    // The knob changes durability, not format: a manifest written with fsync on
    // must load exactly like one written without it.
    {
        chl::ArchiveManifest manifest(root.string(), /*fsync_each_append=*/true);
        ASSERT_EQ(manifest.append(published(1, 0 * NS, 10 * NS)), chl::CL_SUCCESS);
        ASSERT_EQ(manifest.append(published(1, 10 * NS, 20 * NS)), chl::CL_SUCCESS);
    }

    chl::ArchiveManifest reopened(root.string());
    ASSERT_EQ(reopened.load(), chl::CL_SUCCESS);
    EXPECT_EQ(reopened.records().size(), 2u);
    EXPECT_EQ(reopened.deriveWatermarks().at(1), 20 * NS);
}
