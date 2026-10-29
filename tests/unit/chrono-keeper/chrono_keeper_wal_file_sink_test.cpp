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

TEST_F(WalFileSinkTest, WriteAtWithNegativeIovcntReturnsEinval)
{
    auto sink = openSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    std::string const data = "event";
    iovec iov[1] = {iovOf(data)};
    EXPECT_EQ(sink->writeAt(fd, 0, iov, -1), EINVAL);
    ASSERT_EQ(sink->closeFile(fd), 0);

    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_TRUE(allZero(content, 0, content.size()));
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

// Each list call reads the directory through its own descriptor; with a
// shared offset, concurrent calls would split the entries between them.
TEST_F(WalFileSinkTest, ConcurrentListsEachSeeEveryEntry)
{
    auto sink = openSink();
    // enough entries that one readdir buffer does not hold them all
    std::size_t const count = 2000;
    for(std::size_t i = 0; i < count; ++i)
    {
        std::ofstream(fs::path(dir) / ("entry-with-a-long-name-" + std::to_string(i)));
    }

    std::atomic<int> wrong{0};
    std::vector<std::thread> listers;
    for(int t = 0; t < 4; ++t)
    {
        listers.emplace_back(
                [&]
                {
                    for(int round = 0; round < 50; ++round)
                    {
                        std::vector<std::string> names;
                        if(sink->list(names) != 0 || names.size() != count)
                        {
                            ++wrong;
                        }
                    }
                });
    }
    for(auto& lister: listers) { lister.join(); }
    EXPECT_EQ(wrong.load(), 0);
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

TEST_F(WalFileSinkTest, FailNextSyncsWithFailsOnlyThatMany)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->failNextSyncsWith(EIO, 2);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncData(fd), 0);

    // they come before failSyncsWith, which applies again after them
    sink->failSyncsWith(ENOSPC);
    sink->failNextSyncsWith(EIO);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncData(fd), ENOSPC);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

// A sync that started before failNextSyncsWith, and waits in holdSyncs, is not
// one of the next ones: the KeeperWal tests fail the group after a held one.
TEST_F(WalFileSinkTest, FailNextSyncsWithSparesASyncAlreadyHeld)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->holdSyncs();
    std::atomic<int> held_result{-1};
    std::thread held([&]() { held_result = sink->syncData(fd); });
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(sink->syncCount() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(sink->syncCount(), 1u);
    sink->failNextSyncsWith(EIO);
    sink->releaseSyncs();
    held.join();
    EXPECT_EQ(held_result.load(), 0);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->syncData(fd), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

// The first bytes of the buffers reach the file, the rest do not, and the
// call fails; once, and only at that offset.
TEST_F(WalFileSinkTest, FailNextWriteAtWritesItsFirstBytesThenFails)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 64), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->failNextWriteAt(8, 6, EIO);
    std::string const first = "abcd";
    std::string const second = "efgh";
    iovec iov[2] = {iovOf(first), iovOf(second)};
    EXPECT_EQ(sink->writeAt(fd, 0, iov, 2), 0);
    EXPECT_EQ(sink->writeAt(fd, 8, iov, 2), EIO);
    std::string content;
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_EQ(content.substr(0, 8), "abcdefgh");
    EXPECT_EQ(content.substr(8, 6), "abcdef");
    EXPECT_TRUE(allZero(content, 14, content.size()));

    EXPECT_EQ(sink->writeAt(fd, 8, iov, 2), 0);
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_EQ(content.substr(8, 8), "abcdefgh");

    // with no bytes the call writes nothing
    sink->failNextWriteAt(32, 0, ENOSPC);
    EXPECT_EQ(sink->writeAt(fd, 32, iov, 2), ENOSPC);
    ASSERT_EQ(sink->readFile("seg", content), 0);
    EXPECT_TRUE(allZero(content, 16, content.size()));
    EXPECT_EQ(sink->closeFile(fd), 0);
}

TEST_F(WalFileSinkTest, DirectorySyncsAreCountedApartFromDataSyncs)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    EXPECT_EQ(sink->syncDirectory(), 0);
    EXPECT_EQ(sink->syncDirectory(), 0);
    EXPECT_EQ(sink->syncData(fd), 0);
    EXPECT_EQ(sink->directorySyncCount(), 2u);
    // the fdatasync inside createZeroed is not seen by the fault sink
    EXPECT_EQ(sink->syncCount(), 1u);
    EXPECT_EQ(sink->closeFile(fd), 0);
}

TEST_F(WalFileSinkTest, FailDirectorySyncsWithEioLeavesDataSyncsAlone)
{
    auto sink = openFaultSink();
    ASSERT_EQ(sink->createZeroed("seg", 4096), 0);
    int fd = -1;
    ASSERT_EQ(sink->openForWrite("seg", fd), 0);

    sink->failDirectorySyncsWith(EIO);
    EXPECT_EQ(sink->syncDirectory(), EIO);
    EXPECT_EQ(sink->syncData(fd), 0);
    EXPECT_EQ(sink->directorySyncCount(), 1u);

    // and failSyncsWith leaves directory syncs alone
    sink->failDirectorySyncsWith(0);
    sink->failSyncsWith(EIO);
    EXPECT_EQ(sink->syncDirectory(), 0);
    EXPECT_EQ(sink->syncData(fd), EIO);
    EXPECT_EQ(sink->directorySyncCount(), 2u);
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
