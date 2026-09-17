// StoryChunkWriter owns how a window reaches the archive, and both of the
// things it does with a file matter on a shared file system:
//
//   - it writes through a temporary name and renames into place, so a reader on
//     another node never opens a half-written file, and a write that fails
//     leaves nothing behind. A partial file under the window's own name would
//     be picked up by the player's directory listing and fail every replay of
//     that window, even after a re-sent chunk was written successfully to a
//     numbered sibling;
//   - it opens files with HDF5's advisory locking turned off. The grapher
//     writes while players on other nodes read, and on NFS without a lock
//     daemon, or a PFS without full byte-range locking, that lock turns into
//     spurious open failures rather than protection. One process writes a given
//     window file and readers only ever read, so the lock buys nothing here.

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <H5Cpp.h>

#include <chrono_monitor.h>
#include <HDF5FileAccess.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

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
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "story_chunk_writer_test_logger");
        done = true;
    }
}

class ChunkWriter: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        dir = fs::temp_directory_path() / ("chronolog_chunk_writer_test_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override { fs::remove_all(dir); }

    static chl::StoryChunk window(uint64_t start_secs, int events, std::size_t payload)
    {
        chl::StoryChunk chunk("C", "S", kStory, start_secs * NS, (start_secs + 30) * NS);
        for(int i = 0; i < events; ++i)
        {
            chunk.insertEvent(chl::LogEvent(kStory, start_secs * NS + 1 + i, 1, i, std::string(payload, 'x')));
        }
        return chunk;
    }

    std::vector<std::string> fileNames() const
    {
        std::vector<std::string> names;
        for(auto const& entry: fs::directory_iterator(dir)) { names.push_back(entry.path().filename().string()); }
        std::sort(names.begin(), names.end());
        return names;
    }

    fs::path dir;
};
} // namespace

TEST_F(ChunkWriter, ASuccessfulWriteLeavesOnlyTheWindowFile)
{
    chl::StoryChunkWriter writer(dir.string(), "story_chunks", "data");
    chl::StoryChunk chunk = window(60, 4, 16);

    EXPECT_GT(writer.writeStoryChunk(chunk), 0u);

    EXPECT_EQ(fileNames(), (std::vector<std::string>{"C.S.60.vlen.h5"}));
}

// A write that fails midway must not leave the window's name pointing at a file
// the player cannot read.
TEST_F(ChunkWriter, AFailedWriteLeavesNothingBehind)
{
    // in a child: cap how much this process may write to any file, so the
    // dataset write fails partway with EFBIG rather than raising SIGXFSZ
    pid_t const child = ::fork();
    ASSERT_NE(child, -1);
    if(child == 0)
    {
        ::signal(SIGXFSZ, SIG_IGN);
        rlimit const capped{4096, 4096};
        ::setrlimit(RLIMIT_FSIZE, &capped);
        chl::StoryChunkWriter writer(dir.string(), "story_chunks", "data");
        chl::StoryChunk chunk = window(60, 400, 512);
        hsize_t const written = writer.writeStoryChunk(chunk);
        ::_exit(written == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << "the capped write was expected to fail";

    // no window file for a replay to trip over, and no leftover temporary
    EXPECT_TRUE(fileNames().empty()) << "left behind: " << fileNames().front();
}

TEST_F(ChunkWriter, ArchiveFilesAreOpenedWithoutFileLocking)
{
    hbool_t use_file_locking = true;
    hbool_t ignore_when_disabled = false;
    H5::FileAccPropList const fapl = chl::archiveFileAccess();

    ASSERT_GE(H5Pget_file_locking(fapl.getId(), &use_file_locking, &ignore_when_disabled), 0);
    EXPECT_FALSE(use_file_locking);
    // a VFD that cannot honour the setting must not turn it into an open failure
    EXPECT_TRUE(ignore_when_disabled);
}

// The reader opens the same files the writer produced; this is the round trip
// both halves of the locking change have to keep working.
TEST_F(ChunkWriter, AWrittenWindowCanBeOpenedForReadingWhileAnotherHandleIsOpen)
{
    chl::StoryChunkWriter writer(dir.string(), "story_chunks", "data");
    chl::StoryChunk chunk = window(60, 4, 16);
    ASSERT_GT(writer.writeStoryChunk(chunk), 0u);

    std::string const path = (dir / "C.S.60.vlen.h5").string();
    H5::H5File first(path, H5F_ACC_RDONLY, H5::FileCreatPropList::DEFAULT, chl::archiveFileAccess());
    EXPECT_NO_THROW({
        H5::H5File second(path, H5F_ACC_RDONLY, H5::FileCreatPropList::DEFAULT, chl::archiveFileAccess());
        second.close();
    });
    first.close();
}
