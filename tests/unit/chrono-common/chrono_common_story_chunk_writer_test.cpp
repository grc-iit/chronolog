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

#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

    // every write's events are on disk: one file per write, each holding the
    // event count of a different chunk (the writes carried 1, 2, ... events)
    void expectOneFilePerWrite(int writes) const
    {
        std::vector<std::string> const names = fileNames();
        ASSERT_EQ(names.size(), static_cast<std::size_t>(writes)) << "files: " << ::testing::PrintToString(names);
        std::vector<hsize_t> event_counts;
        for(std::string const& name: names)
        {
            H5::H5File file((dir / name).string(),
                            H5F_ACC_RDONLY,
                            H5::FileCreatPropList::DEFAULT,
                            chl::archiveFileAccess());
            H5::DataSet const dataset = file.openDataSet("/story_chunks/data.vlen_bytes");
            event_counts.push_back(dataset.getSpace().getSimpleExtentNpoints());
        }
        std::sort(event_counts.begin(), event_counts.end());
        for(int i = 0; i < writes; ++i) { EXPECT_EQ(event_counts[i], static_cast<hsize_t>(1 + i)); }
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

// Two writes of one window can be in flight at once: the grapher runs more than
// one extraction stream, and a late or re-sent chunk writes its window again.
// Each write that reports success has settled its receipts, so it must leave a
// file of its own; one renamed over another loses the first one's events.
TEST_F(ChunkWriter, ConcurrentWritesOfOneWindowEachKeepTheirOwnFile)
{
    constexpr int kWriters = 12;
    std::vector<chl::StoryChunk> chunks;
    for(int i = 0; i < kWriters; ++i) { chunks.push_back(window(60, 1 + i, 16)); }

    std::atomic<int> waiting{kWriters};
    std::atomic<int> succeeded{0};
    std::vector<std::thread> writers;
    for(int i = 0; i < kWriters; ++i)
    {
        writers.emplace_back(
                [&, i]()
                {
                    chl::StoryChunkWriter writer(dir.string(), "story_chunks", "data");
                    // start together, so the writes overlap
                    --waiting;
                    while(waiting.load() > 0) { std::this_thread::yield(); }
                    if(writer.writeStoryChunk(chunks[i]) > 0)
                    {
                        ++succeeded;
                    }
                });
    }
    for(auto& writer: writers) { writer.join(); }
    ASSERT_EQ(succeeded.load(), kWriters);

    expectOneFilePerWrite(kWriters);
}

// The same across processes. The visor gives a story acquired anew a random
// recording group, so a story released and acquired again within one window
// has two graphers, on different nodes, writing that window to the shared
// archive. A lock inside one process does not reach the other.
TEST_F(ChunkWriter, ConcurrentWritesOfOneWindowFromSeparateProcessesEachKeepTheirOwnFile)
{
    constexpr int kWriters = 12;
    int go[2];
    ASSERT_EQ(::pipe(go), 0);
    std::vector<pid_t> children;
    for(int i = 0; i < kWriters; ++i)
    {
        pid_t const child = ::fork();
        ASSERT_NE(child, -1);
        if(child == 0)
        {
            ::close(go[1]);
            char byte = 0;
            // returns once the parent closes its end, so the writes start together
            (void)!::read(go[0], &byte, 1);
            chl::StoryChunkWriter writer(dir.string(), "story_chunks", "data");
            chl::StoryChunk chunk = window(60, 1 + i, 16);
            ::_exit(writer.writeStoryChunk(chunk) > 0 ? 0 : 1);
        }
        children.push_back(child);
    }
    ::close(go[0]);
    ::close(go[1]);
    for(pid_t const child: children)
    {
        int status = 0;
        ::waitpid(child, &status, 0);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0) << "a write failed";
    }

    expectOneFilePerWrite(kWriters);
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
