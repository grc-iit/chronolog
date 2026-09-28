// Unit tests for StoryChunkExtractorCSV. The grapher hands every window to its
// extractors, empty ones included, because the HDF5 extractor needs an empty
// window to move the persisted watermark past an idle gap. A CSV file per
// empty window would pile up on the shared file system for every idle story.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include <abt.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ChunkExtractorCSV.h>
#include <ServiceId.h>
#include <StoryChunk.h>

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
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "chunk_extractor_csv_test_logger");
        done = true;
    }
}

// the extractor logs the Argobots thread id of its caller
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

class ChunkExtractorCSV: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        ensureArgobots();
        dir = fs::temp_directory_path() / ("chronolog_csv_extractor_test_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override { fs::remove_all(dir); }

    std::size_t fileCount() const
    {
        std::size_t count = 0;
        for(auto const& entry: fs::directory_iterator(dir))
        {
            (void)entry;
            ++count;
        }
        return count;
    }

    fs::path dir;
};
} // namespace

TEST_F(ChunkExtractorCSV, AnEmptyChunkWritesNoFile)
{
    chl::StoryChunkExtractorCSV extractor(chl::ServiceId{}, dir.string());
    chl::StoryChunk window("C", "S", kStory, 60 * NS, 90 * NS);

    EXPECT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    EXPECT_EQ(fileCount(), 0u);
}

TEST_F(ChunkExtractorCSV, AChunkWithEventsWritesItsFile)
{
    chl::StoryChunkExtractorCSV extractor(chl::ServiceId{}, dir.string());
    chl::StoryChunk window("C", "S", kStory, 60 * NS, 90 * NS);
    window.insertEvent(chl::LogEvent(kStory, 60 * NS + 1, 1, 0, "event"));

    EXPECT_EQ(extractor.process_chunk(&window), chl::CL_SUCCESS);
    EXPECT_EQ(fileCount(), 1u);
}
