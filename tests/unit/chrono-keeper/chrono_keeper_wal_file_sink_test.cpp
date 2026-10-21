// Unit tests for WalFileSink, the file layer under the keeper WAL: the POSIX
// sink against a temporary directory, and the fault-injecting test sink that
// later WAL tests use to fail or stall writes and syncs.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <climits>
#include <sys/stat.h>

#include <thallium.hpp>

#include <WalFileSink.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{

class WalFileSinkTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string pattern = (fs::temp_directory_path() / "wal_file_sink_XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        dir = (fs::path(root) / "wal").string();
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    std::unique_ptr<chl::WalFileSink> openSink()
    {
        auto sink = chl::makePosixWalFileSink();
        EXPECT_EQ(sink->openDirectory(dir), 0);
        return sink;
    }

    std::unique_ptr<chl::FaultInjectingWalFileSink> openFaultSink()
    {
        auto sink = std::make_unique<chl::FaultInjectingWalFileSink>(chl::makePosixWalFileSink());
        EXPECT_EQ(sink->openDirectory(dir), 0);
        return sink;
    }

    std::string root;
    std::string dir;
};

iovec iovOf(std::string const& s) { return iovec{const_cast<char*>(s.data()), s.size()}; }

bool allZero(std::string const& s, std::size_t from, std::size_t to)
{
    for(std::size_t i = from; i < to; ++i)
    {
        if(s[i] != '\0')
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_F(WalFileSinkTest, OpenDirectoryCreatesMissingParents)
{
    dir = (fs::path(root) / "a" / "b" / "wal").string();
    auto sink = openSink();
    EXPECT_TRUE(fs::is_directory(dir));
}

TEST_F(WalFileSinkTest, CallsBeforeOpenDirectoryReturnEbadf)
{
    auto sink = chl::makePosixWalFileSink();
    EXPECT_EQ(sink->lockDirectory(), EBADF);
    EXPECT_EQ(sink->syncDirectory(), EBADF);
    std::vector<std::string> names;
    EXPECT_EQ(sink->list(names), EBADF);
}

TEST_F(WalFileSinkTest, SecondLockFromAnotherSinkReturnsEwouldblock)
{
    auto first = openSink();
    ASSERT_EQ(first->lockDirectory(), 0);
    EXPECT_TRUE(fs::exists(fs::path(dir) / "lock"));

    auto second = openSink();
    EXPECT_EQ(second->lockDirectory(), EWOULDBLOCK);

    // the lock goes with the sink that holds it
    first.reset();
    EXPECT_EQ(second->lockDirectory(), 0);
}

TEST_F(WalFileSinkTest, CreateZeroedMakesFileOfExactSizeAllZeros)
{
    auto sink = openSink();
    // not a multiple of any write buffer
    uint64_t const bytes = (3u << 20) + 123;
    ASSERT_EQ(sink->createZeroed("00000000000000000001.wal.partial", bytes), 0);

    std::string content;
    ASSERT_EQ(sink->readFile("00000000000000000001.wal.partial", content), 0);
    ASSERT_EQ(content.size(), bytes);
    EXPECT_TRUE(allZero(content, 0, content.size()));
}

TEST_F(WalFileSinkTest, CreateZeroedReplacesLongerFileWithZeros)
{
    auto sink = openSink();
    {
        std::ofstream out(fs::path(dir) / "seg", std::ios::binary);
        out << std::string(10000, 'x');
    }
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);

    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    ASSERT_EQ(content.size(), 4096u);
    EXPECT_TRUE(allZero(content, 0, content.size()));
}

TEST_F(WalFileSinkTest, WriteAtWithThreeIovecsLandsContiguously)
{
    auto sink = openSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    std::string const a = "head", b = "record-bytes", c = "tail";
    iovec iov[3] = {iovOf(a), iovOf(b), iovOf(c)};
    ASSERT_EQ(sink->writeAt(fd, 100, iov, 3), 0);
    ASSERT_EQ(sink->syncData(fd), 0);
    ASSERT_EQ(sink->closeFile(fd), 0);

    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    // an overwrite in place does not grow the file
    ASSERT_EQ(content.size(), 4096u);
    std::string const expected = a + b + c;
    EXPECT_EQ(content.substr(100, expected.size()), expected);
    EXPECT_TRUE(allZero(content, 0, 100));
    EXPECT_TRUE(allZero(content, 100 + expected.size(), content.size()));
}

TEST_F(WalFileSinkTest, WriteAtSplitsMoreIovecsThanIovMax)
{
    auto sink = openSink();
    ASSERT_EQ(sink->createZeroed("seg", 8192), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    int const count = 3 * IOV_MAX + 7;
    std::string bytes(count, '\0');
    std::vector<iovec> iov(count);
    for(int i = 0; i < count; ++i)
    {
        bytes[i] = static_cast<char>('a' + i % 26);
        iov[i] = iovec{&bytes[i], 1};
    }
    ASSERT_EQ(sink->writeAt(fd, 10, iov.data(), count), 0);
    ASSERT_EQ(sink->closeFile(fd), 0);

    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_EQ(content.substr(10, count), bytes);
}

TEST_F(WalFileSinkTest, OpenForWriteOfMissingFileReturnsEnoent)
{
    auto sink = openSink();
    int fd = -1;
    EXPECT_EQ(sink->openForWrite("missing", fd), ENOENT);
}

TEST_F(WalFileSinkTest, RenameRemoveListAndSyncDirectory)
{
    auto sink = openSink();
    ASSERT_EQ(sink->createZeroed("00000000000000000002.wal.partial", 16), 0);
    ASSERT_EQ(sink->createZeroed("00000000000000000001.wal", 16), 0);
    ASSERT_EQ(sink->rename("00000000000000000002.wal.partial", "00000000000000000002.wal"), 0);
    ASSERT_EQ(sink->syncDirectory(), 0);

    std::vector<std::string> names;
    ASSERT_EQ(sink->list(names), 0);
    EXPECT_EQ(names, (std::vector<std::string>{"00000000000000000001.wal", "00000000000000000002.wal"}));

    ASSERT_EQ(sink->remove("00000000000000000001.wal"), 0);
    EXPECT_EQ(sink->remove("00000000000000000001.wal"), ENOENT);
    ASSERT_EQ(sink->list(names), 0);
    EXPECT_EQ(names, (std::vector<std::string>{"00000000000000000002.wal"}));

    std::string content;
    EXPECT_EQ(sink->readFile("00000000000000000001.wal", content), ENOENT);
}

TEST_F(WalFileSinkTest, FreeSpaceReportsTheFileSystem)
{
    auto sink = openSink();
    uint64_t free_bytes = 0, total_bytes = 0;
    ASSERT_EQ(sink->freeSpace(free_bytes, total_bytes), 0);
    EXPECT_GT(total_bytes, 0u);
    EXPECT_LE(free_bytes, total_bytes);
}

TEST_F(WalFileSinkTest, FailWritesWithEnospcReturnsItAndWritesNothing)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->failWritesWith(ENOSPC);
    std::string const data = "event";
    iovec iov[1] = {iovOf(data)};
    EXPECT_EQ(sink->writeAt(fd, 0, iov, 1), ENOSPC);
    EXPECT_EQ(sink->createZeroed("spare", 4096), ENOSPC);
    EXPECT_FALSE(fs::exists(fs::path(dir) / "spare"));

    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_TRUE(allZero(content, 0, content.size()));

    sink->failWritesWith(0);
    EXPECT_EQ(sink->writeAt(fd, 0, iov, 1), 0);
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_EQ(content.substr(0, data.size()), data);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

TEST_F(WalFileSinkTest, FailSyncsWithEioReturnsItOnEveryLaterSync)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    EXPECT_EQ(sink->syncData(fd), 0);
    sink->failSyncsWith(EIO);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncCount(), 3u);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

TEST_F(WalFileSinkTest, SetFreeSpaceOverridesTheFileSystem)
{
    auto sink = openFaultSink();
    sink->setFreeSpace(1000, 5000);
    uint64_t free_bytes = 0, total_bytes = 0;
    ASSERT_EQ(sink->freeSpace(free_bytes, total_bytes), 0);
    EXPECT_EQ(free_bytes, 1000u);
    EXPECT_EQ(total_bytes, 5000u);
}

TEST_F(WalFileSinkTest, HoldSyncsBlocksASyncUntilReleased)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->holdSyncs();
    std::atomic<bool> done{false};
    std::atomic<int> result{-1};
    std::thread syncer(
            [&]
            {
                result = sink->syncData(fd);
                done = true;
            });

    while(sink->syncCount() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(done.load());

    sink->releaseSyncs();
    syncer.join();
    EXPECT_TRUE(done.load());
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

// The committer runs on its own Argobots stream; a held sync must yield, so
// other threads on that stream keep running while it waits.
TEST_F(WalFileSinkTest, HeldSyncYieldsToOtherArgobotsThreadsOnItsStream)
{
    thallium::abt argobots;
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->holdSyncs();
    std::atomic<bool> syncDone{false};
    std::atomic<bool> otherRan{false};
    {
        thallium::managed<thallium::xstream> stream = thallium::xstream::create();
        thallium::managed<thallium::thread> syncer = stream->make_thread(
                [&]
                {
                    sink->syncData(fd);
                    syncDone = true;
                });
        // created second, so it runs on the stream only if the held sync yields
        thallium::managed<thallium::thread> other = stream->make_thread([&] { otherRan = true; });

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!otherRan.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(otherRan.load());
        EXPECT_FALSE(syncDone.load());

        sink->releaseSyncs();
        syncer->join();
        other->join();
        stream->join();
    }
    EXPECT_TRUE(syncDone.load());
    EXPECT_EQ(sink->closeFile(fd), 0);
}
