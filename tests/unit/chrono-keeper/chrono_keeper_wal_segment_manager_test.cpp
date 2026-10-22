// Unit tests for WalSegmentManager, which owns the segment files of a keeper
// WAL directory: the lock, the cleanup of half-prepared segments, the
// pre-zeroed spare with its H record, and the deletion of released segments.
// Runs against a temporary directory through the fault-injecting sink.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <cerrno>
#include <sys/resource.h>

#include <thallium.hpp>

#include <WalFileSink.h>
#include <WalFormat.h>
#include <WalSegmentManager.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t kSegmentBytes = 1u << 20;
std::string const kEndpoint = "ofi+sockets://127.0.0.1:6666";

class WalSegmentManagerTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string pattern = (fs::temp_directory_path() / "wal_segment_manager_XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        dir = (fs::path(root) / "wal").string();
        sink = std::make_unique<chl::FaultInjectingWalFileSink>(chl::makePosixWalFileSink());
        ASSERT_EQ(sink->openDirectory(dir), 0);
    }

    void TearDown() override
    {
        sink.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    std::unique_ptr<chl::WalSegmentManager> makeManager(uint64_t segment_bytes = kSegmentBytes)
    {
        return std::make_unique<chl::WalSegmentManager>(*sink, segment_bytes, kEndpoint, dir);
    }

    std::string readFile(std::string const& name)
    {
        std::ifstream in(fs::path(dir) / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeFile(std::string const& name, std::string const& bytes)
    {
        std::ofstream out(fs::path(dir) / name, std::ios::binary);
        out << bytes;
    }

    bool exists(std::string const& name) { return fs::exists(fs::path(dir) / name); }

    // the segment's H record, checked to be the first frame
    wal::HeaderRecord headerOf(std::string const& bytes)
    {
        wal::ScanResult scan = wal::scanSegment(bytes);
        wal::HeaderRecord header{};
        EXPECT_FALSE(scan.records.empty());
        if(scan.records.empty())
        {
            return header;
        }
        EXPECT_EQ(scan.records[0].offset, 0u);
        EXPECT_EQ(scan.records[0].type, static_cast<uint8_t>(wal::RecordType::Header));
        EXPECT_EQ(wal::decodeHeader(scan.records[0].body, header), 0);
        return header;
    }

    // a header-only segment as a preparer would have left it
    void writeHeaderOnlySegment(uint64_t segment, uint64_t bytes = kSegmentBytes)
    {
        std::string data = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, segment, 1, kEndpoint});
        data.resize(bytes, '\0');
        writeFile(wal::segmentFileName(segment), data);
    }

    thallium::abt argobots;
    std::string root;
    std::string dir;
    std::unique_ptr<chl::FaultInjectingWalFileSink> sink;
};

bool allZero(std::string const& s, std::size_t from)
{
    for(std::size_t i = from; i < s.size(); ++i)
    {
        if(s[i] != '\0')
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_F(WalSegmentManagerTest, OpenRefusesALockedDirectory)
{
    auto other = chl::makePosixWalFileSink();
    ASSERT_EQ(other->openDirectory(dir), 0);
    ASSERT_EQ(other->lockDirectory(), 0);
    writeFile("00000000000000000002.wal.partial", "half");

    auto manager = makeManager();
    EXPECT_EQ(manager->open(), EWOULDBLOCK);
    EXPECT_NE(manager->error().find(dir), std::string::npos) << manager->error();
    // refused before touching anything
    EXPECT_TRUE(exists("00000000000000000002.wal.partial"));
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    EXPECT_NE(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
}

TEST_F(WalSegmentManagerTest, OpenRemovesPartialFiles)
{
    writeHeaderOnlySegment(1);
    writeHeaderOnlySegment(2);
    writeFile("00000000000000000003.wal.partial", std::string(4096, '\0'));
    writeFile("stray.partial", "x");

    auto manager = makeManager();
    uint64_t const syncs_before = sink->directorySyncCount();
    ASSERT_EQ(manager->open(), 0);
    EXPECT_FALSE(exists("00000000000000000003.wal.partial"));
    EXPECT_FALSE(exists("stray.partial"));
    EXPECT_TRUE(exists(wal::segmentFileName(1)));
    EXPECT_TRUE(exists(wal::segmentFileName(2)));
    EXPECT_GT(sink->directorySyncCount(), syncs_before);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{1, 2}));

    // the next segment comes after the ones found
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(segment, 3u);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(sink->closeFile(fd), 0);
}

TEST_F(WalSegmentManagerTest, EverySegmentStartsWithAHeader)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    manager->startPreparer();

    std::vector<uint64_t> taken;
    for(int i = 0; i < 4; ++i)
    {
        uint64_t segment = 0;
        uint64_t data_offset = 0;
        uint64_t size = 0;
        int fd = -1;
        ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset, &size), 0);
        EXPECT_EQ(size, kSegmentBytes);
        EXPECT_EQ(sink->closeFile(fd), 0);
        taken.push_back(segment);

        std::string bytes = readFile(wal::segmentFileName(segment));
        wal::HeaderRecord header = headerOf(bytes);
        EXPECT_EQ(header.format_version, wal::kFormatVersion);
        EXPECT_EQ(header.segment_number, segment);
        EXPECT_EQ(header.keeper_endpoint, kEndpoint);
        EXPECT_GT(header.created_ns, 0u);
        EXPECT_EQ(data_offset, wal::scanSegment(bytes).records[0].body.size() + wal::kFrameHeaderBytes);
    }
    manager->stopPreparer();
    EXPECT_EQ(taken, (std::vector<uint64_t>{1, 2, 3, 4}));
    EXPECT_EQ(manager->segments(), taken);
}

TEST_F(WalSegmentManagerTest, SpareIsZeroAfterTheHeader)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    manager->startPreparer();
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);

    // the preparer makes the next spare in the background: wait for it, no
    // partial left behind
    std::string const next = wal::segmentFileName(segment + 1);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!exists(next) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    manager->stopPreparer();
    ASSERT_TRUE(exists(next));
    EXPECT_FALSE(exists(next + ".partial"));

    for(std::string const& name: {wal::segmentFileName(segment), next})
    {
        std::string bytes = readFile(name);
        ASSERT_EQ(bytes.size(), kSegmentBytes) << name;
        wal::ScanResult scan = wal::scanSegment(bytes);
        ASSERT_EQ(scan.records.size(), 1u) << name;
        EXPECT_EQ(scan.end, wal::ScanEnd::EndOfData);
        EXPECT_TRUE(allZero(bytes, scan.end_offset)) << name;
    }
    // the ready spare is not listed until it is taken
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{segment}));
}

TEST_F(WalSegmentManagerTest, TakeSpareLargerThanSegmentCreatesOneOfThatSize)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    manager->startPreparer();

    std::vector<uint64_t> taken;
    auto take = [&](uint64_t min_bytes)
    {
        uint64_t segment = 0;
        uint64_t data_offset = 0;
        uint64_t size = 0;
        int fd = -1;
        ASSERT_EQ(manager->takeSpare(min_bytes, segment, fd, data_offset, &size), 0);
        EXPECT_EQ(sink->closeFile(fd), 0);
        taken.push_back(segment);

        std::string bytes = readFile(wal::segmentFileName(segment));
        EXPECT_EQ(size, min_bytes);
        EXPECT_EQ(bytes.size(), min_bytes);
        EXPECT_EQ(headerOf(bytes).segment_number, segment);
        EXPECT_TRUE(allZero(bytes, data_offset));
    };

    take(kSegmentBytes);
    ASSERT_EQ(taken.size(), 1u);

    // once the preparer has numbered and made the next regular spare, the
    // oversized take must grow that spare in place rather than prepare one
    std::string const next = wal::segmentFileName(taken[0] + 1);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!exists(next) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(exists(next));

    // stopped, so the preparer does not sync segment n+2 while the count is
    // taken; the spare it made stays ready
    manager->stopPreparer();
    uint64_t const big = 3 * kSegmentBytes + 4096;
    uint64_t const syncs_before = sink->syncCount();
    take(big);
    ASSERT_EQ(taken.size(), 2u);
    EXPECT_EQ(taken[1], taken[0] + 1);
    // the grown size is committed before anything is written there
    EXPECT_EQ(sink->syncCount(), syncs_before + 1);

    manager->startPreparer();
    take(kSegmentBytes);
    manager->stopPreparer();
    // numbers keep log order across the oversized one
    ASSERT_EQ(taken.size(), 3u);
    EXPECT_LT(taken[1], taken[2]);
    EXPECT_EQ(manager->segments(), taken);
}

TEST_F(WalSegmentManagerTest, DeleteRemovesAndSyncsTheDirectory)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    std::vector<uint64_t> taken;
    for(int i = 0; i < 3; ++i)
    {
        uint64_t segment = 0;
        uint64_t data_offset = 0;
        int fd = -1;
        ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
        EXPECT_EQ(sink->closeFile(fd), 0);
        taken.push_back(segment);
    }

    uint64_t const syncs_before = sink->directorySyncCount();
    ASSERT_EQ(manager->deleteSegment(taken[0]), 0);
    EXPECT_FALSE(exists(wal::segmentFileName(taken[0])));
    EXPECT_EQ(sink->directorySyncCount(), syncs_before + 1);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{taken[1], taken[2]}));

    // a segment the manager does not list is left alone
    EXPECT_EQ(manager->deleteSegment(taken[0]), ENOENT);
    EXPECT_EQ(manager->deleteSegment(999), ENOENT);
    EXPECT_EQ(sink->directorySyncCount(), syncs_before + 1);
    EXPECT_TRUE(exists(wal::segmentFileName(taken[1])));
}

TEST_F(WalSegmentManagerTest, NamesSortInNumericOrder)
{
    std::vector<uint64_t> const numbers = {99999999999ull, 9, 10000000000ull, 10, 9999999999ull};
    for(uint64_t n: numbers) { writeHeaderOnlySegment(n, 4096); }
    // not segments
    writeFile("exit-summary", "pending_events=0 pending_bytes=0\n");
    writeFile("123.wal", "");
    writeFile("00000000000000000007.wal.bak", "");

    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{9, 10, 9999999999ull, 10000000000ull, 99999999999ull}));

    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    EXPECT_EQ(segment, 100000000000ull);
    EXPECT_EQ(wal::segmentFileName(segment), "00000000100000000000.wal");

    // a fresh listing of the directory sorts the same way
    std::vector<std::string> names;
    ASSERT_EQ(sink->list(names), 0);
    std::vector<uint64_t> listed;
    for(std::string const& name: names)
    {
        if(auto n = wal::parseSegmentFileName(name))
        {
            listed.push_back(*n);
        }
    }
    EXPECT_EQ(listed, manager->segments());
}

TEST_F(WalSegmentManagerTest, AdoptExistingReusesAHeaderOnlyNewestSegment)
{
    writeHeaderOnlySegment(4);
    writeHeaderOnlySegment(5, 2 * kSegmentBytes);

    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    int fd = -1;
    uint64_t data_offset = 0;
    uint64_t size = 0;
    // only the newest can be adopted, or writes would land before older data
    EXPECT_EQ(manager->adoptExisting(4, fd, data_offset), EINVAL);
    ASSERT_EQ(manager->adoptExisting(5, fd, data_offset, &size), 0);
    EXPECT_EQ(size, 2 * kSegmentBytes);
    std::string bytes = readFile(wal::segmentFileName(5));
    EXPECT_EQ(data_offset, wal::scanSegment(bytes).end_offset);

    // the adopted segment's fd writes into the file
    std::string const payload = "after the header";
    iovec iov{const_cast<char*>(payload.data()), payload.size()};
    ASSERT_EQ(sink->writeAt(fd, data_offset, &iov, 1), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    EXPECT_EQ(readFile(wal::segmentFileName(5)).substr(data_offset, payload.size()), payload);

    // the next spare follows it
    uint64_t segment = 0;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    EXPECT_EQ(segment, 6u);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{4, 5, 6}));
}

TEST_F(WalSegmentManagerTest, AdoptExistingRefusesASegmentWithRecords)
{
    std::string data = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, 1, 1, kEndpoint});
    data += wal::encodeDropped(wal::DroppedRecord{42});
    data.resize(kSegmentBytes, '\0');
    writeFile(wal::segmentFileName(1), data);

    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    int fd = -1;
    uint64_t data_offset = 0;
    EXPECT_EQ(manager->adoptExisting(1, fd, data_offset), EINVAL);
    EXPECT_EQ(readFile(wal::segmentFileName(1)), data);
}

// A preparation that fails is reported to the taker, not retried in a loop or
// left as a hang, and leaves no partial file; the next take after the fault
// clears succeeds.
TEST_F(WalSegmentManagerTest, FailedPreparationIsReportedAndCleanedUp)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    sink->failWritesWith(ENOSPC);
    manager->startPreparer();

    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    EXPECT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), ENOSPC);
    EXPECT_TRUE(manager->segments().empty());
    std::vector<std::string> names;
    ASSERT_EQ(sink->list(names), 0);
    for(std::string const& name: names) { EXPECT_EQ(name, "lock"); }

    sink->failWritesWith(0);
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    manager->stopPreparer();
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{segment}));
    EXPECT_EQ(headerOf(readFile(wal::segmentFileName(segment))).segment_number, segment);
}

// Taken from an Argobots thread on another stream, as the committer does,
// while the preparer's syncs are held: the taker waits without blocking its
// stream and gets the spare once the syncs are released.
TEST_F(WalSegmentManagerTest, TakeSpareWaitsForThePreparerOnArgobots)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    sink->holdSyncs();
    manager->startPreparer();

    std::atomic<int> result{-1};
    std::atomic<bool> otherRan{false};
    uint64_t segment = 0;
    {
        thallium::managed<thallium::xstream> stream = thallium::xstream::create();
        thallium::managed<thallium::thread> taker = stream->make_thread(
                [&]
                {
                    uint64_t data_offset = 0;
                    int fd = -1;
                    result = manager->takeSpare(kSegmentBytes, segment, fd, data_offset);
                    if(result == 0)
                    {
                        sink->closeFile(fd);
                    }
                });
        thallium::managed<thallium::thread> other = stream->make_thread([&] { otherRan = true; });

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!otherRan.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(otherRan.load());
        EXPECT_EQ(result.load(), -1);

        sink->releaseSyncs();
        taker->join();
        other->join();
        stream->join();
    }
    manager->stopPreparer();
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(segment, 1u);
}

namespace
{

// segment 1 holding a D record after its header, as pending events would
std::string segmentWithRecords()
{
    std::string data = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, 1, 1, kEndpoint});
    data += wal::encodeDropped(wal::DroppedRecord{42});
    data.resize(kSegmentBytes, '\0');
    return data;
}

} // namespace

// A preparer started on a manager that is not open would number its spare 1
// and rename it over an existing segment 1.
TEST_F(WalSegmentManagerTest, StartPreparerRefusesBeforeOpen)
{
    std::string const data = segmentWithRecords();
    writeFile(wal::segmentFileName(1), data);

    auto manager = makeManager();
    EXPECT_FALSE(manager->isOpen());
    manager->startPreparer();
    EXPECT_FALSE(manager->isOpen());
    EXPECT_NE(manager->error().find("is not open"), std::string::npos) << manager->error();
    EXPECT_NE(manager->error().find(dir), std::string::npos) << manager->error();
    // long enough for a preparer to replace a 1 MiB segment
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    manager->stopPreparer();

    EXPECT_EQ(readFile(wal::segmentFileName(1)), data);
    EXPECT_FALSE(exists(wal::segmentFileName(1) + ".partial"));
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    EXPECT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), EBADF);
}

TEST_F(WalSegmentManagerTest, StartPreparerRefusesAfterAFailedOpen)
{
    std::string const data = segmentWithRecords();
    writeFile(wal::segmentFileName(1), data);

    // another keeper holds the directory
    auto other = chl::makePosixWalFileSink();
    ASSERT_EQ(other->openDirectory(dir), 0);
    ASSERT_EQ(other->lockDirectory(), 0);

    auto manager = makeManager();
    ASSERT_EQ(manager->open(), EWOULDBLOCK);
    EXPECT_FALSE(manager->isOpen());
    std::string const reason = manager->error();
    manager->startPreparer();
    EXPECT_FALSE(manager->isOpen());
    // still names the lock holder, not only that the directory is not open
    EXPECT_EQ(manager->error(), reason);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    manager->stopPreparer();

    EXPECT_EQ(readFile(wal::segmentFileName(1)), data);
    EXPECT_FALSE(exists(wal::segmentFileName(1) + ".partial"));
    EXPECT_FALSE(exists(wal::segmentFileName(2)));
}

// A second open() on an open manager does not list the directory again or
// remove partial files, which by then could be the preparer's.
TEST_F(WalSegmentManagerTest, SecondOpenChangesNothing)
{
    writeHeaderOnlySegment(1);
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);
    EXPECT_TRUE(manager->isOpen());

    writeHeaderOnlySegment(7);
    writeFile("00000000000000000002.wal.partial", std::string(4096, '\0'));
    EXPECT_EQ(manager->open(), 0) << manager->error();
    EXPECT_TRUE(manager->isOpen());
    EXPECT_TRUE(exists("00000000000000000002.wal.partial"));
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{1}));

    uint64_t segment = 0;
    uint64_t data_offset = 0;
    int fd = -1;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    EXPECT_EQ(segment, 2u);
}

namespace
{

// Forwards to another sink and records the Argobots stream each file was
// zero-filled on, which is where the zero-fill and its fdatasync ran.
class StreamRecordingSink: public chl::WalFileSink
{
public:
    explicit StreamRecordingSink(chl::WalFileSink& inner)
        : inner(inner)
    {}

    // file name -> rank of the stream that called createZeroed
    std::map<std::string, int> zeroedOn()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return ranks;
    }

    // After the next syncDirectory, a ULT queued on the calling stream blocks
    // that stream for `stall` once the caller yields, so a ULT woken there
    // meanwhile does not run until it ends.
    void stallStreamAfterNextDirectorySync(std::chrono::milliseconds stall)
    {
        stallMs = stall.count();
        stallArmed = true;
    }

    int openDirectory(std::string const& dir) override { return inner.openDirectory(dir); }
    int lockDirectory() override { return inner.lockDirectory(); }
    int createZeroed(std::string const& name, uint64_t bytes) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ranks[name] = thallium::xstream::self_rank();
        }
        return inner.createZeroed(name, bytes);
    }
    int openForWrite(std::string const& name, int& fd) override { return inner.openForWrite(name, fd); }
    int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) override
    {
        return inner.writeAt(fd, offset, iov, iovcnt);
    }
    int syncData(int fd) override { return inner.syncData(fd); }
    int rename(std::string const& from, std::string const& to) override { return inner.rename(from, to); }
    int remove(std::string const& name) override { return inner.remove(name); }
    int syncDirectory() override
    {
        int err = inner.syncDirectory();
        if(stallArmed.exchange(false))
        {
            std::chrono::milliseconds const stall(stallMs.load());
            thallium::xstream::self().make_thread([stall] { std::this_thread::sleep_for(stall); },
                                                  thallium::anonymous());
        }
        return err;
    }
    int readFile(std::string const& name, std::string& out) override { return inner.readFile(name, out); }
    int list(std::vector<std::string>& names) override { return inner.list(names); }
    int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) override
    {
        return inner.freeSpace(free_bytes, total_bytes);
    }
    int closeFile(int fd) override { return inner.closeFile(fd); }

private:
    chl::WalFileSink& inner;
    std::mutex mutex;
    std::map<std::string, int> ranks;
    std::atomic<bool> stallArmed{false};
    std::atomic<int64_t> stallMs{0};
};

} // namespace

// A take issued right after another, before the preparer has woken to make
// the next spare, waits for the preparer rather than zero-filling and syncing
// a whole segment on the taker's stream, where it would stall the durable
// writers waiting on the committer. The preparer's stream is kept busy after it
// makes segment 1, so the second take always comes before it wakes.
TEST_F(WalSegmentManagerTest, TakeSpareRightAfterATakeWaitsForThePreparer)
{
    StreamRecordingSink recording(*sink);
    chl::WalSegmentManager manager(recording, kSegmentBytes, kEndpoint, dir);
    ASSERT_EQ(manager.open(), 0);
    recording.stallStreamAfterNextDirectorySync(std::chrono::milliseconds(500));
    manager.startPreparer();
    // the stall starts once segment 1 is renamed into place and handed over
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!exists(wal::segmentFileName(1)) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(exists(wal::segmentFileName(1)));

    std::vector<uint64_t> taken;
    std::vector<int> fds;
    int result = 0;
    int takerRank = -1;
    {
        thallium::managed<thallium::xstream> stream = thallium::xstream::create();
        thallium::managed<thallium::thread> taker = stream->make_thread(
                [&]
                {
                    takerRank = thallium::xstream::self_rank();
                    for(int i = 0; i < 2 && result == 0; ++i)
                    {
                        uint64_t segment = 0;
                        uint64_t data_offset = 0;
                        int fd = -1;
                        if((result = manager.takeSpare(kSegmentBytes, segment, fd, data_offset)) == 0)
                        {
                            taken.push_back(segment);
                            fds.push_back(fd);
                        }
                    }
                });
        taker->join();
        stream->join();
    }
    manager.stopPreparer();
    for(int fd: fds) { EXPECT_EQ(sink->closeFile(fd), 0); }

    ASSERT_EQ(result, 0);
    ASSERT_EQ(taken, (std::vector<uint64_t>{1, 2}));
    std::map<std::string, int> const zeroed = recording.zeroedOn();
    std::string const first = wal::segmentFileName(1) + ".partial";
    std::string const second = wal::segmentFileName(2) + ".partial";
    ASSERT_EQ(zeroed.count(first), 1u);
    ASSERT_EQ(zeroed.count(second), 1u);
    EXPECT_NE(zeroed.at(first), takerRank);
    // made by the preparer, as segment 1 was, not on the taker's stream
    EXPECT_EQ(zeroed.at(second), zeroed.at(first));
    EXPECT_NE(zeroed.at(second), takerRank) << "segment 2 was zero-filled on the taker's stream";
}

// When the preparer's stream cannot be created, startPreparer reports it and
// leaves the manager as if it had not been called: a take prepares on its own
// thread instead of waiting for a preparer that does not exist, and a later
// startPreparer starts it.
TEST_F(WalSegmentManagerTest, PreparerThatCannotStartLeavesTakeSpareWorking)
{
    auto manager = makeManager();
    ASSERT_EQ(manager->open(), 0);

    // with RLIMIT_NPROC at 0, pthread_create fails with EAGAIN, and so does
    // the creation of the preparer's stream
    rlimit saved{};
    ASSERT_EQ(getrlimit(RLIMIT_NPROC, &saved), 0);
    rlimit none = saved;
    none.rlim_cur = 0;
    ASSERT_EQ(setrlimit(RLIMIT_NPROC, &none), 0);
    bool threadsRefused = false;
    try
    {
        std::thread([] {}).join();
    }
    catch(std::system_error const&)
    {
        threadsRefused = true;
    }
    if(!threadsRefused)
    {
        ASSERT_EQ(setrlimit(RLIMIT_NPROC, &saved), 0);
        GTEST_SKIP() << "threads are still created under RLIMIT_NPROC 0 (a privileged user)";
    }
    EXPECT_NO_THROW(manager->startPreparer());
    ASSERT_EQ(setrlimit(RLIMIT_NPROC, &saved), 0);
    EXPECT_NE(manager->error().find("preparer"), std::string::npos) << manager->error();
    EXPECT_NE(manager->error().find(dir), std::string::npos) << manager->error();

    // taken on another stream so that a take waiting forever fails the test
    // instead of hanging it
    std::atomic<int> result{-1};
    uint64_t segment = 0;
    {
        thallium::managed<thallium::xstream> stream = thallium::xstream::create();
        thallium::managed<thallium::thread> taker = stream->make_thread(
                [&]
                {
                    uint64_t data_offset = 0;
                    int fd = -1;
                    int err = manager->takeSpare(kSegmentBytes, segment, fd, data_offset);
                    if(err == 0)
                    {
                        sink->closeFile(fd);
                    }
                    result = err;
                });
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(result.load() == -1 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_NE(result.load(), -1) << "takeSpare waits for a preparer that never started";
        if(result.load() == -1)
        {
            // a preparer that does start hands the waiting take its spare
            manager->startPreparer();
        }
        taker->join();
        stream->join();
    }
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(segment, 1u);

    manager->startPreparer();
    uint64_t data_offset = 0;
    int fd = -1;
    ASSERT_EQ(manager->takeSpare(kSegmentBytes, segment, fd, data_offset), 0);
    EXPECT_EQ(sink->closeFile(fd), 0);
    manager->stopPreparer();
    EXPECT_EQ(segment, 2u);
    EXPECT_EQ(manager->segments(), (std::vector<uint64_t>{1, 2}));
}
