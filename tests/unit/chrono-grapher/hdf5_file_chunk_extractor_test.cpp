// Unit tests for the HDF5FileChunkExtractor -> StoryWatermarkRegistry seam.
// The extractor is the only caller that advances the grapher's persisted
// watermark W, and it must do so only for a merged timeline window that is
// actually on disk. A watermark-exempt chunk (the StoryPipeline salvage path:
// one keeper's rescued events, not a merged window) is written but must not
// move W. If it did, W would claim durability over a range that was never
// fully persisted and the keepers would free chunks they still need.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

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
