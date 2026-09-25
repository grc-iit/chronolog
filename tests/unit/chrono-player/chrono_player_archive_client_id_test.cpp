// A client id is 64 bits wide, but the HDF5 archive stored only its lower 32
// bits. An event replayed from the archive then came back with a different
// client id than the same event held by a keeper, so the two copies could not
// be recognized as one event. Files written before the fix keep their 32-bit
// ids and still have to be readable.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <list>
#include <string>
#include <unistd.h>

#include <H5Cpp.h>
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
constexpr uint64_t kWindowStart = 10 * NS;
constexpr uint64_t kWindowEnd = 11 * NS;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "archive_client_id_test_logger");
        done = true;
    }
}

// The record layout of every archive file written before client ids were
// widened: a 32-bit clientId at offset 16.
struct LegacyEventRecord
{
    uint64_t storyId;
    uint64_t eventTime;
    uint32_t clientId;
    uint32_t eventIndex;
    hvl_t logRecord;
};

class ArchiveClientId: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        archiveDir =
                fs::temp_directory_path() / ("chronolog_archive_client_id_test_" + std::to_string(::getpid()) + "_" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(archiveDir);
        fs::create_directories(archiveDir);
    }

    void TearDown() override
    {
        for(chl::StoryChunk* chunk: chunks) { delete chunk; }
        fs::remove_all(archiveDir);
    }

    // Everything the archive returns for the test window.
    void readWindow()
    {
        chl::HDF5ArchiveReadingAgent agent(archiveDir.string(), true, std::chrono::milliseconds(100));
        agent.initialize();
        agent.readArchivedStory("chron", "story", kWindowStart, kWindowEnd, chunks);
        agent.shutdown();
    }

    // the agent logs thallium::thread::self_id(), which needs Argobots
    thallium::abt argobots;
    fs::path archiveDir;
    std::list<chl::StoryChunk*> chunks;
};
} // namespace

TEST_F(ArchiveClientId, ArchivedEventKeepsItsFullClientId)
{
    constexpr chl::ClientId kWideClientId = 0x7F00000012345678ULL;
    chl::StoryChunk chunk("chron", "story", kStory, kWindowStart, kWindowEnd);
    chunk.insertEvent(chl::LogEvent(kStory, kWindowStart + 1, kWideClientId, 3, "payload"));
    chl::StoryChunkWriter writer(archiveDir.string(), "story_chunks", "data");
    ASSERT_GT(writer.writeStoryChunk(chunk), 0u);

    readWindow();
    ASSERT_EQ(chunks.size(), 1u);
    ASSERT_EQ(chunks.front()->getEventCount(), 1);
    EXPECT_EQ(chunks.front()->begin()->second.clientId, kWideClientId);
}

TEST_F(ArchiveClientId, FileWithThe32BitLayoutStillReads)
{
    std::string const payload = "payload";
    LegacyEventRecord record{kStory, kWindowStart + 1, 0x12345678u, 3, {payload.size(), (void*)payload.data()}};
    H5::CompType type(sizeof(LegacyEventRecord));
    type.insertMember("storyId", HOFFSET(LegacyEventRecord, storyId), H5::PredType::NATIVE_UINT64);
    type.insertMember("eventTime", HOFFSET(LegacyEventRecord, eventTime), H5::PredType::NATIVE_UINT64);
    type.insertMember("clientId", HOFFSET(LegacyEventRecord, clientId), H5::PredType::NATIVE_UINT32);
    type.insertMember("eventIndex", HOFFSET(LegacyEventRecord, eventIndex), H5::PredType::NATIVE_UINT32);
    type.insertMember("logRecord", HOFFSET(LegacyEventRecord, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));
    {
        H5::H5File file((archiveDir / "chron.story.10.vlen.h5").string(), H5F_ACC_TRUNC);
        H5::Group group = file.createGroup("story_chunks");
        hsize_t dims = 1;
        H5::DataSpace space(1, &dims);
        H5::DataSet dataset = file.createDataSet("/story_chunks/data.vlen_bytes", type, space);
        dataset.write(&record, type);
    }

    readWindow();
    ASSERT_EQ(chunks.size(), 1u);
    ASSERT_EQ(chunks.front()->getEventCount(), 1);
    EXPECT_EQ(chunks.front()->begin()->second.clientId, 0x12345678u);
    EXPECT_EQ(chunks.front()->begin()->second.getRecord(), payload);
}
