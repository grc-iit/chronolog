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
#include <string>
#include <thread>
#include <vector>

#include <cerrno>

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

    uint64_t const big = 3 * kSegmentBytes + 4096;
    std::vector<uint64_t> taken;
    for(uint64_t min_bytes: {kSegmentBytes, big, kSegmentBytes})
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
    }
    manager->stopPreparer();
    // numbers keep log order across the oversized one
    EXPECT_LT(taken[0], taken[1]);
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
