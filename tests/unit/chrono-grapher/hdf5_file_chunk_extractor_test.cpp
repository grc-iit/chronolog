// Unit tests for the HDF5FileChunkExtractor -> StoryWatermarkRegistry seam.
// The extractor is the only caller that advances the grapher's persisted
// watermark W, and it must do so only for a merged timeline window that is
// actually on disk. A watermark-exempt chunk (the StoryPipeline salvage path:
// one keeper's rescued events, not a merged window) is written but must not
// move W. If it did, W would claim durability over a range that was never
// fully persisted and the keepers would free chunks they still need.

#include <gtest/gtest.h>

#include <algorithm>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <typeinfo>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <H5Cpp.h>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <HDF5FileChunkExtractor.h>
#include <StoryChunk.h>
#include <StoryWatermarkRegistry.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{
constexpr chl::StoryId kStory = 42;
constexpr uint64_t NS = 1000000000ULL;

// Window boundaries on whole seconds: the extractor names files by start second.
constexpr uint64_t T0 = 10 * NS;
constexpr uint64_t T1 = 11 * NS;
constexpr uint64_t T2 = 12 * NS;

void addEvent(chl::StoryChunk& chunk, uint64_t time, uint32_t index)
{
    chunk.insertEvent(chl::LogEvent(kStory, time, 1, index, "payload"));
}

class HDF5FileChunkExtractorWatermark: public ::testing::Test
{
protected:
    void SetUp() override
    {
        archiveDir = fs::temp_directory_path() / ("chronolog_hdf5_extractor_test_" + std::to_string(::getpid()) + "_" +
                                                  ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
        extractor.reset(archiveDir.string());
        extractor.attachWatermarkRegistry(&registry);
        registry.registerStory(kStory, T0);
    }

    void TearDown() override { fs::remove_all(archiveDir); }

    std::size_t archivedFileCount() const
    {
        std::size_t count = 0;
        for(auto const& entry: fs::directory_iterator(archiveDir))
        {
            if(entry.is_regular_file() && entry.path().extension() == ".h5")
            {
                count++;
            }
        }
        return count;
    }

    // process_chunk logs thallium::thread::self_id(), which throws outside an
    // Argobots context; tl::abt initializes Argobots without a Mercury engine.
    thallium::abt argobots;
    fs::path archiveDir;
    chl::StoryWatermarkRegistry registry;
    chl::HDF5FileChunkExtractor extractor;
};
} // namespace

TEST_F(HDF5FileChunkExtractorWatermark, WrittenWindowAdvancesW)
{
    chl::StoryChunk window("C", "S", kStory, T0, T1);
    addEvent(window, T0 + 1, 0);

    EXPECT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    EXPECT_EQ(archivedFileCount(), 1u);
    EXPECT_EQ(registry.getPersisted(kStory), T1);
}

TEST_F(HDF5FileChunkExtractorWatermark, EmptyWindowAdvancesWWithoutWritingAFile)
{
    // An idle-gap window is vacuously durable; without it the contiguous
    // prefix would hold at the gap forever.
    chl::StoryChunk window("C", "S", kStory, T0, T1);

    EXPECT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    EXPECT_EQ(archivedFileCount(), 0u);
    EXPECT_EQ(registry.getPersisted(kStory), T1);
}

TEST_F(HDF5FileChunkExtractorWatermark, WatermarkExemptChunkIsWrittenButDoesNotAdvanceW)
{
    chl::StoryChunk salvage("C", "S", kStory, T0, T2);
    salvage.setWatermarkExempt(true);
    addEvent(salvage, T0 + 1, 0);

    EXPECT_EQ(extractor.process_chunk(&salvage), chl::CL_SUCCESS);
    // the salvaged events are persisted...
    EXPECT_EQ(archivedFileCount(), 1u);
    // ...but their interval proves nothing about the rest of [T0, T2)
    EXPECT_EQ(registry.getPersisted(kStory), T0);
}

TEST_F(HDF5FileChunkExtractorWatermark, WatermarkExemptChunkLeavesItsIntervalAGap)
{
    // The exempt interval must not even be parked as persisted: a merged
    // window above it may not let W leap over the salvage range.
    chl::StoryChunk salvage("C", "S", kStory, T0, T1);
    salvage.setWatermarkExempt(true);
    addEvent(salvage, T0 + 1, 0);
    ASSERT_EQ(extractor.process_chunk(&salvage), chl::CL_SUCCESS);

    chl::StoryChunk window("C", "S", kStory, T1, T2);
    addEvent(window, T1 + 1, 0);
    ASSERT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);

    EXPECT_EQ(registry.getPersisted(kStory), T0);
}

TEST_F(HDF5FileChunkExtractorWatermark, EmptyWatermarkExemptChunkDoesNotAdvanceW)
{
    chl::StoryChunk salvage("C", "S", kStory, T0, T1);
    salvage.setWatermarkExempt(true);

    EXPECT_EQ(extractor.process_chunk(&salvage), chl::CL_SUCCESS);
    EXPECT_EQ(registry.getPersisted(kStory), T0);
}

TEST_F(HDF5FileChunkExtractorWatermark, FailedWriteHoldsWAndRecordsTheFailure)
{
    extractor.reset((archiveDir / "missing").string());
    chl::StoryChunk window("C", "S", kStory, T0, T1);
    addEvent(window, T0 + 1, 0);

    EXPECT_NE(extractor.process_chunk(&window), chl::CL_SUCCESS);
    EXPECT_EQ(registry.getPersisted(kStory), T0);

    // The failure is recorded: a later fresh re-registration may not cover
    // the gap, or the keepers would free the failed window's chunks.
    registry.registerStory(kStory, T2, /*fresh_pipeline=*/true);
    EXPECT_EQ(registry.getPersisted(kStory), T0);
}

// ---- receipts ---------------------------------------------------------------
//
// A written chunk releases the receipts of the keeper chunks whose events it
// holds, whether or not it moves W: a salvage chunk is exactly the case W
// cannot confirm. A failed write releases nothing, so those keepers keep their
// chunks and send them again.

TEST_F(HDF5FileChunkExtractorWatermark, WrittenWindowReleasesItsReceipts)
{
    uint64_t const receipt = registry.assignReceipt(kStory);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    chl::StoryChunk window("C", "S", kStory, T0, T1);
    addEvent(window, T0 + 1, 0);
    window.carryReceipt(receipt);

    ASSERT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    auto snapshot = registry.snapshotDirty();
    ASSERT_EQ(snapshot.count(kStory), 1u);
    EXPECT_EQ(snapshot.at(kStory).highest_receipt, receipt);
    EXPECT_TRUE(snapshot.at(kStory).pending_receipts.empty());
}

TEST_F(HDF5FileChunkExtractorWatermark, WrittenSalvageChunkReleasesItsReceipts)
{
    uint64_t const receipt = registry.assignReceipt(kStory);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    chl::StoryChunk salvage("C", "S", kStory, T0, T1);
    salvage.setWatermarkExempt(true);
    addEvent(salvage, T0 + 1, 0);
    salvage.carryReceipt(receipt);

    ASSERT_EQ(extractor.process_chunk(&salvage), chl::CL_SUCCESS);
    auto snapshot = registry.snapshotDirty();
    ASSERT_EQ(snapshot.count(kStory), 1u);
    EXPECT_TRUE(snapshot.at(kStory).pending_receipts.empty());
}

TEST_F(HDF5FileChunkExtractorWatermark, EmptyWindowReleasesItsReceipts)
{
    // a window can hold a receipt and still arrive empty: its events sorted
    // into the neighbouring window, which holds the receipt too. The empty one
    // is still a holder, so it has to let go or the receipt never settles.
    uint64_t const receipt = registry.assignReceipt(kStory);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    chl::StoryChunk window("C", "S", kStory, T0, T1);
    window.carryReceipt(receipt);

    ASSERT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    auto snapshot = registry.snapshotDirty();
    ASSERT_EQ(snapshot.count(kStory), 1u);
    EXPECT_TRUE(snapshot.at(kStory).pending_receipts.empty());
}

TEST_F(HDF5FileChunkExtractorWatermark, FailedWriteKeepsItsReceiptsPending)
{
    extractor.reset((archiveDir / "missing").string());
    uint64_t const receipt = registry.assignReceipt(kStory);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    chl::StoryChunk window("C", "S", kStory, T0, T1);
    addEvent(window, T0 + 1, 0);
    window.carryReceipt(receipt);

    EXPECT_NE(extractor.process_chunk(&window), chl::CL_SUCCESS);
    // registration in SetUp left the story dirty, so a report is due
    auto snapshot = registry.snapshotDirty();
    ASSERT_EQ(snapshot.count(kStory), 1u);
    EXPECT_EQ(snapshot.at(kStory).pending_receipts, (std::vector<uint64_t>{receipt}));
}

// A late chunk -- one that arrived after the story's W had already passed its
// range -- lands in a reopened past window below W. For it E <= W already holds,
// so W cannot hold it back: the receipt is the only thing telling the keeper its
// events are not on disk. If that window's write fails, the receipt must stay
// pending; a keeper that saw it as settled would free the chunk once the
// visibility delay passed, and the events would exist nowhere.
TEST_F(HDF5FileChunkExtractorWatermark, FailedWriteOfALateChunkKeepsItsReceiptPending)
{
    // the story's W is already past the late chunk's range
    registry.advancePersisted(kStory, T0, T2);
    ASSERT_GE(registry.getPersisted(kStory), T1);

    extractor.reset((archiveDir / "missing").string());
    uint64_t const receipt = registry.assignReceipt(kStory);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    chl::StoryChunk reopened("C", "S", kStory, T0, T1);
    addEvent(reopened, T0 + 1, 0);
    reopened.carryReceipt(receipt);
    (void)registry.snapshotDirty();

    EXPECT_NE(extractor.process_chunk(&reopened), chl::CL_SUCCESS);

    // W still covers the chunk -- nothing moves it back -- so everything rests on
    // the receipt being reported as unwritten
    EXPECT_GE(registry.getPersisted(kStory), T1);
    auto snapshot = registry.snapshotDirty();
    auto const report = snapshot.find(kStory);
    bool const reported_pending =
            report != snapshot.end() &&
            std::find(report->second.pending_receipts.begin(), report->second.pending_receipts.end(), receipt) !=
                    report->second.pending_receipts.end();
    // with no fresh report, the keeper keeps the last one it had, which listed it
    bool const never_cleared = report == snapshot.end();
    EXPECT_TRUE(reported_pending || never_cleared)
            << "the receipt of a late chunk whose write failed was cleared; its keeper would free events that are "
               "on disk nowhere";
}

// ---- a write that runs out of room ------------------------------------------
//
// A write can fail at any step: creating the file, writing the dataset,
// flushing or closing the file. Whichever step fails, the extractor must return
// an error rather than throw, and W must not move past a window whose events
// cannot be read back. The test caps how many bytes may be written to a file,
// so that each step in turn runs out of room. Each write runs in a child
// process, which alone carries the cap.

namespace
{
// The number of events read back from the HDF5 file under dir, or -1 if there
// is no file or it cannot be read in full.
long readableEventCount(fs::path const& dir)
{
    for(auto const& entry: fs::directory_iterator(dir))
    {
        if(entry.path().extension() != ".h5")
        {
            continue;
        }
        try
        {
            H5::H5File file(entry.path().string(), H5F_ACC_RDONLY);
            H5::DataSet dataset = file.openDataSet("/story_chunks/data.vlen_bytes");
            H5::DataSpace space = dataset.getSpace();
            hsize_t count = 0;
            space.getSimpleExtentDims(&count);
            H5::DataType file_type = dataset.getDataType();
            hid_t const memory_type = H5Tget_native_type(file_type.getId(), H5T_DIR_ASCEND);
            std::vector<char> buffer(H5Tget_size(memory_type) * count);
            herr_t const status = H5Dread(dataset.getId(), memory_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data());
            if(status >= 0)
            {
                H5Dvlen_reclaim(memory_type, space.getId(), H5P_DEFAULT, buffer.data());
            }
            H5Tclose(memory_type);
            return (status < 0) ? -1 : static_cast<long>(count);
        }
        catch(H5::Exception const&)
        {
            return -1;
        }
    }
    return -1;
}

enum class CappedWrite
{
    HeldW,
    MovedW,
    Threw,
    Crashed
};

// Writes a window of `events` events to dir with the extractor, in a child
// process that may write at most `cap` bytes to a file. A write past the cap
// fails with EFBIG instead of raising SIGXFSZ.
CappedWrite writeUnderCap(fs::path const& dir, rlim_t cap, long events)
{
    pid_t const child = ::fork();
    if(child == 0)
    {
        int outcome = 3;
        try
        {
            chl::StoryWatermarkRegistry capped_registry;
            chl::HDF5FileChunkExtractor capped_extractor;
            capped_extractor.reset(dir.string());
            capped_extractor.attachWatermarkRegistry(&capped_registry);
            capped_registry.registerStory(kStory, T0);
            chl::StoryChunk window("C", "S", kStory, T0, T1);
            for(long i = 0; i < events; ++i)
            {
                window.insertEvent(chl::LogEvent(kStory, T0 + 1 + i, 1, i, std::string(200, 'x')));
            }
            std::signal(SIGXFSZ, SIG_IGN);
            rlimit const capped{cap, cap};
            ::setrlimit(RLIMIT_FSIZE, &capped);
            capped_extractor.process_chunk(&window);
            outcome = (capped_registry.getPersisted(kStory) == T1) ? 1 : 0;
        }
        catch(H5::Exception const& error)
        {
            std::cout << "cap " << cap << " child threw " << typeid(error).name() << " in " << error.getFuncName()
                      << ": " << error.getDetailMsg() << std::endl;
            outcome = 2;
        }
        catch(std::exception const& error)
        {
            std::cout << "cap " << cap << " child threw " << typeid(error).name() << ": " << error.what() << std::endl;
            outcome = 2;
        }
        catch(...)
        {
            std::cout << "cap " << cap << " child threw something else" << std::endl;
            outcome = 2;
        }
        ::_exit(outcome);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    if(!WIFEXITED(status))
    {
        return CappedWrite::Crashed;
    }
    switch(WEXITSTATUS(status))
    {
        case 0:
            return CappedWrite::HeldW;
        case 1:
            return CappedWrite::MovedW;
        case 2:
            return CappedWrite::Threw;
        default:
            return CappedWrite::Crashed;
    }
}
} // namespace

TEST_F(HDF5FileChunkExtractorWatermark, WMovesOnlyPastAWindowThatReadsBack)
{
    constexpr long kEvents = 64;
    std::size_t moved = 0;
    std::size_t held = 0;
    for(rlim_t cap = 0; cap <= 64 * 1024; cap += 512)
    {
        fs::path const dir = archiveDir / std::to_string(cap);
        fs::create_directories(dir);
        CappedWrite const outcome = writeUnderCap(dir, cap, kEvents);
        EXPECT_NE(outcome, CappedWrite::Threw) << "file size cap " << cap << " bytes";
        EXPECT_NE(outcome, CappedWrite::Crashed) << "file size cap " << cap << " bytes";
        if(outcome == CappedWrite::MovedW)
        {
            ++moved;
            EXPECT_EQ(readableEventCount(dir), kEvents) << "file size cap " << cap << " bytes";
        }
        else if(outcome == CappedWrite::HeldW)
        {
            ++held;
        }
    }
    // the sweep covers both a cap too small to write and one large enough
    EXPECT_GT(held, 0u);
    EXPECT_GT(moved, 0u);
}
