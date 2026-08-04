// StoryChunkWriter's publish step: an archive file must become visible under its
// final name only once it is complete, and publishing must never destroy a file
// that is already there.
//
// Both properties matter because of who else is looking at the directory. The
// Player discovers archive content by scanning for "*.h5" and reading whatever it
// finds, so a file created empty and filled in afterwards is a file the Player can
// read mid-write. And extraction runs on several streams at once
// (StoryChunkExtractionModule's stream_count, 2 by default), so two chunks for the
// same story and window-start second can be published concurrently -- a publish
// that overwrote the destination would silently drop one of them.
//
// The writer therefore builds under a temporary name that does not end in ".h5"
// (so the Player's extension check skips it) and publishes by creating a hard link
// at the final name, which fails rather than clobbers if that name is taken.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_types.h>

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

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "story_chunk_writer_test_logger");
        done = true;
    }
}

class StoryChunkWriterTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        root = fs::temp_directory_path() / ("chronolog_writer_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    chl::StoryChunk makeChunk(int events = 3)
    {
        chl::StoryChunk chunk("chron", "story", STORY_ID, CHUNK_START, CHUNK_END);
        for(int i = 0; i < events; i++)
        {
            chunk.insertEvent(
                    chl::LogEvent(STORY_ID, CHUNK_START + (uint64_t)(i + 1), 7, (chl::chrono_index)i, "payload"));
        }
        return chunk;
    }

    std::vector<std::string> archiveFiles() const
    {
        std::vector<std::string> names;
        for(auto const& entry: fs::directory_iterator(root)) { names.push_back(entry.path().filename().string()); }
        std::sort(names.begin(), names.end());
        return names;
    }

    fs::path root;
};

} // namespace

TEST_F(StoryChunkWriterTest, PublishesUnderTheFinalNameAndLeavesNoTemporaryBehind)
{
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunk chunk = makeChunk();

    chl::StoryChunkWriteResult const result = writer.writeStoryChunk(chunk);

    EXPECT_GT(result.file_size, 0u);
    std::vector<std::string> const files = archiveFiles();
    ASSERT_EQ(files.size(), 1u) << "a temporary file was left in the archive directory";
    EXPECT_EQ(files.front(), "chron.story.1736800000.vlen.h5");
}

TEST_F(StoryChunkWriterTest, ReportsThePublishedFileNameAndRotationIndex)
{
    // The manifest records the file it must later find, so the writer has to say
    // which name it actually chose rather than leaving the caller to re-derive it.
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunk chunk = makeChunk();

    chl::StoryChunkWriteResult const result = writer.writeStoryChunk(chunk);

    EXPECT_EQ(fs::path(result.file_name).filename().string(), "chron.story.1736800000.vlen.h5");
    EXPECT_EQ(result.seq, 0u);
    EXPECT_TRUE(fs::exists(root / fs::path(result.file_name).filename()));
}

TEST_F(StoryChunkWriterTest, ASecondChunkForTheSameWindowRotatesInsteadOfOverwriting)
{
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunk first = makeChunk();
    chl::StoryChunk second = makeChunk();

    chl::StoryChunkWriteResult const first_result = writer.writeStoryChunk(first);
    chl::StoryChunkWriteResult const second_result = writer.writeStoryChunk(second);

    EXPECT_EQ(first_result.seq, 0u);
    EXPECT_EQ(second_result.seq, 1u) << "a re-send must land beside the original, not on top of it";
    EXPECT_NE(first_result.file_name, second_result.file_name);
    EXPECT_EQ(archiveFiles().size(), 2u);
}

TEST_F(StoryChunkWriterTest, PublishRefusesToReplaceAnExistingFile)
{
    // The real hazard is the window between choosing a free name and publishing
    // it: extraction runs on several streams, so another stream can take that name
    // in between. A publish built on rename() would replace the file it found and
    // silently destroy an already-archived chunk, so the publish step itself has
    // to fail on collision. Driving writeStoryChunk() cannot show this -- name
    // selection sees the existing file and rotates before publish is ever reached
    // -- so the publish step is exercised directly.
    fs::path const source = root / "complete.tmp";
    {
        std::ofstream out(source);
        out << "freshly written chunk";
    }
    fs::path const occupied = root / "chron.story.1736800000.vlen.h5";
    {
        std::ofstream out(occupied);
        out << "already archived";
    }
    auto const original_size = fs::file_size(occupied);

    EXPECT_FALSE(chl::StoryChunkWriter::publishFile(source.string(), occupied.string()))
            << "publish must fail rather than replace an existing archive file";
    EXPECT_EQ(fs::file_size(occupied), original_size) << "the pre-existing file was overwritten";

    // ... and it does publish when the name is free.
    fs::path const free_name = root / "chron.story.1736800000.vlen.1.h5";
    EXPECT_TRUE(chl::StoryChunkWriter::publishFile(source.string(), free_name.string()));
    EXPECT_TRUE(fs::exists(free_name));
}

TEST_F(StoryChunkWriterTest, AnExistingFileForTheSameWindowIsRotatedAroundNotThroughs)
{
    // The end-to-end counterpart: a file already occupying the base name makes the
    // writer pick the next rotation index instead of touching it.
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    fs::path const occupied = root / "chron.story.1736800000.vlen.h5";
    {
        std::ofstream squatter(occupied);
        squatter << "not an hdf5 file";
    }
    auto const original_size = fs::file_size(occupied);

    chl::StoryChunk chunk = makeChunk();
    chl::StoryChunkWriteResult const result = writer.writeStoryChunk(chunk);

    ASSERT_GT(result.file_size, 0u);
    EXPECT_EQ(fs::file_size(occupied), original_size);
    EXPECT_EQ(result.seq, 1u);
}

TEST_F(StoryChunkWriterTest, TheTemporaryNameIsNotDiscoverableAsAnArchiveFile)
{
    // The Player's isValidArchiveFile() accepts a path purely on its ".h5"
    // extension, so an in-progress file must not carry one.
    std::string const temp_name = chl::StoryChunkWriter::temporaryFileNameFor("chron.story.1736800000.vlen.h5");

    EXPECT_NE(fs::path(temp_name).extension(), ".h5")
            << "an in-progress file would be picked up by the Player: " << temp_name;
    EXPECT_NE(temp_name, "chron.story.1736800000.vlen.h5");
}

TEST_F(StoryChunkWriterTest, AnEmptyChunkWritesNoFile)
{
    chl::StoryChunkWriter writer(root.string() + "/", "story_chunks", "data");
    chl::StoryChunk empty("chron", "story", STORY_ID, CHUNK_START, CHUNK_END);

    chl::StoryChunkWriteResult const result = writer.writeStoryChunk(empty);

    EXPECT_EQ(result.file_size, 0u);
    EXPECT_TRUE(archiveFiles().empty()) << "an empty chunk must not leave a file or a temporary behind";
}
