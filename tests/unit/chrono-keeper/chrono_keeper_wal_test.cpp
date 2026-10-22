// Unit tests for KeeperWal, the keeper's write-ahead log for durable writes:
// group commit on the committer's own stream, the answer only after the sync,
// the failure latch, the free-space reserve, N records per segment, records
// larger than a segment, retries that join a write in flight, releases from
// the retention store, D records and stop. Runs against a temporary directory
// through the fault-injecting sink, with requests made from ULTs on a pool of
// four streams, the size of the keeper's RPC pool in the shipped template.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <cerrno>
#include <sys/resource.h>

#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <ChronoKeeperConfiguration.h>
#include <KeeperWal.h>
#include <LogEventCodec.h>
#include <StoryChunk.h>
#include <WalFileSink.h>
#include <WalFormat.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;
namespace fs = std::filesystem;
namespace tl = thallium;

using namespace std::chrono_literals;

namespace
{

constexpr uint64_t kMiB = 1u << 20;
std::string const kEndpoint = "ofi+sockets://127.0.0.1:6666";
std::string const kChronicle = "chronicle";
constexpr chl::StoryId kStory = 7;
constexpr chl::StoryId kOtherStory = 8;

std::string storyName(chl::StoryId story) { return "story-" + std::to_string(story); }

chl::LogEvent makeEvent(chl::StoryId story, uint64_t time, std::size_t record_bytes = 64, uint32_t index = 0)
{
    return chl::LogEvent(story, time, 1, index, std::string(record_bytes, static_cast<char>('a' + time % 26)));
}

chl::EventSequence identityOf(chl::LogEvent const& event)
{
    return chl::EventSequence{event.time(), event.getClientId(), event.index()};
}

bool waitFor(std::function<bool()> const& done, std::chrono::milliseconds timeout = 5s)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while(!done())
    {
        if(std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Four streams sharing one pool, like the keeper's RPC pool.
class UltPool
{
public:
    explicit UltPool(int streams = 4)
    {
        pool = tl::pool::create(tl::pool::access::mpmc);
        for(int i = 0; i < streams; ++i)
        {
            xstreams.push_back(tl::xstream::create(tl::scheduler::predef::basic_wait, *pool));
        }
    }

    ~UltPool()
    {
        join();
        for(auto& stream: xstreams) { stream->join(); }
    }

    void run(std::function<void()> work) { threads.push_back(pool->make_thread(std::move(work))); }

    void join()
    {
        for(auto& thread: threads) { thread->join(); }
        threads.clear();
    }

private:
    tl::managed<tl::pool> pool;
    std::vector<tl::managed<tl::xstream>> xstreams;
    std::vector<tl::managed<tl::thread>> threads;
};

// One durable write made from a ULT, and its answer once it has one.
struct Append
{
    chl::LogEvent event;
    std::atomic<bool> done{false};
    std::atomic<int> status{1};
};

// One lookupDurable made from a ULT; answer is set before done.
struct Lookup
{
    chl::LogEvent event;
    std::atomic<bool> done{false};
    std::optional<int> answer;
};

// Forwards to another sink. Fails rename, which only the preparation of a
// spare uses, when told to, and counts the renames tried.
class RenameFailingSink: public chl::WalFileSink
{
public:
    explicit RenameFailingSink(std::unique_ptr<chl::WalFileSink> inner)
        : inner(std::move(inner))
    {}

    void failRenamesWith(int err) { renameError = err; }
    uint64_t renameCount() const { return renames.load(); }

    int openDirectory(std::string const& dir) override { return inner->openDirectory(dir); }
    int lockDirectory() override { return inner->lockDirectory(); }
    int createZeroed(std::string const& name, uint64_t bytes) override { return inner->createZeroed(name, bytes); }
    int openForWrite(std::string const& name, int& fd) override { return inner->openForWrite(name, fd); }
    int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) override
    {
        return inner->writeAt(fd, offset, iov, iovcnt);
    }
    int syncData(int fd) override { return inner->syncData(fd); }
    int rename(std::string const& from, std::string const& to) override
    {
        ++renames;
        if(int err = renameError.load())
        {
            return err;
        }
        return inner->rename(from, to);
    }
    int remove(std::string const& name) override { return inner->remove(name); }
    int syncDirectory() override { return inner->syncDirectory(); }
    int readFile(std::string const& name, std::string& out) override { return inner->readFile(name, out); }
    int list(std::vector<std::string>& names) override { return inner->list(names); }
    int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) override
    {
        return inner->freeSpace(free_bytes, total_bytes);
    }
    int closeFile(int fd) override { return inner->closeFile(fd); }

private:
    std::unique_ptr<chl::WalFileSink> inner;
    std::atomic<int> renameError{0};
    std::atomic<uint64_t> renames{0};
};

// Sets RLIMIT_NPROC to 0 while it lives, so pthread_create fails with EAGAIN
// and no Argobots stream can be created. refused() is false when threads are
// still created under the limit (a privileged user).
class NoNewThreads
{
public:
    NoNewThreads()
    {
        if(getrlimit(RLIMIT_NPROC, &saved) != 0)
        {
            return;
        }
        rlimit none = saved;
        none.rlim_cur = 0;
        if(setrlimit(RLIMIT_NPROC, &none) != 0)
        {
            return;
        }
        limited = true;
        try
        {
            std::thread([] {}).join();
        }
        catch(std::system_error const&)
        {
            threadsRefused = true;
        }
    }

    ~NoNewThreads() { restore(); }

    void restore()
    {
        if(limited)
        {
            setrlimit(RLIMIT_NPROC, &saved);
            limited = false;
        }
    }

    bool refused() const { return threadsRefused; }

private:
    rlimit saved{};
    bool limited = false;
    bool threadsRefused = false;
};

// A record read back from a segment file.
struct OnDisk
{
    uint64_t segment;
    uint8_t type;
    std::string body;
};

class KeeperWalTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string pattern = (fs::temp_directory_path() / "keeper_wal_XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        dir = (fs::path(root) / "wal").string();
        conf.enabled = true;
        conf.dir = dir;
        conf.segment_bytes = kMiB;
    }

    void TearDown() override
    {
        wal.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The sink reports plenty of free space unless a test says otherwise, so
    // the reserve does not depend on the disk the tests run on. With
    // rename_failures the WAL writes through a RenameFailingSink around it.
    void makeWal(std::chrono::milliseconds idle_rotation = 1min, bool rename_failures = false)
    {
        auto owned = std::make_unique<chl::FaultInjectingWalFileSink>(chl::makePosixWalFileSink());
        sink = owned.get();
        sink->setFreeSpace(1000 * kMiB * 1024, 2000 * kMiB * 1024);
        std::unique_ptr<chl::WalFileSink> top = std::move(owned);
        renameSink = nullptr;
        if(rename_failures)
        {
            auto wrapped = std::make_unique<RenameFailingSink>(std::move(top));
            renameSink = wrapped.get();
            top = std::move(wrapped);
        }
        wal = std::make_unique<chl::KeeperWal>(conf, kEndpoint, std::move(top), idle_rotation);
    }

    // In a fresh directory start takes segment 1 and the preparer makes
    // segment 2. Waiting for it keeps the preparer's sync out of the tests'
    // held syncs and sync counts.
    void startWal(std::chrono::milliseconds idle_rotation = 1min, bool rename_failures = false)
    {
        makeWal(idle_rotation, rename_failures);
        ASSERT_EQ(wal->start(chl::RecoveredState{}, {}), chl::CL_SUCCESS);
        ASSERT_TRUE(waitFor([this]() { return segmentNumbers().size() == 2; }));
    }

    int append(chl::LogEvent const& event)
    {
        return wal->appendDurable(kChronicle, storyName(event.getStoryId()), event);
    }

    // Starts a durable write of the request's event on a ULT of the pool.
    void appendOn(UltPool& pool, Append& request)
    {
        pool.run(
                [this, &request]()
                {
                    request.status = append(request.event);
                    request.done = true;
                });
    }

    // Starts a lookupDurable of the request's event on a ULT of the pool.
    void lookupOn(UltPool& pool, Lookup& request)
    {
        pool.run(
                [this, &request]()
                {
                    request.answer = wal->lookupDurable(request.event.getStoryId(), identityOf(request.event));
                    request.done = true;
                });
    }

    // Durable writes of every event from that many ULTs, each writing its
    // share in order, so the groups are large and the run is short. True when
    // every one got CL_SUCCESS.
    bool appendAll(std::vector<chl::LogEvent> const& events, std::size_t writers = 64)
    {
        std::atomic<std::size_t> failures{0};
        {
            UltPool pool;
            for(std::size_t writer = 0; writer < writers; ++writer)
            {
                pool.run(
                        [&, writer]()
                        {
                            for(std::size_t i = writer; i < events.size(); i += writers)
                            {
                                if(append(events[i]) != chl::CL_SUCCESS)
                                {
                                    ++failures;
                                }
                            }
                        });
            }
        }
        return failures.load() == 0;
    }

    // Writes the segment a crashed keeper would have left: its H record, then
    // an N and an E record for each event, zeros to segment_bytes. Returns
    // the recovered state recovery would make of it: one copy of each event,
    // in that segment, pending under the event's story id.
    chl::RecoveredState writeRecoveredSegment(uint64_t segment, std::vector<chl::LogEvent> const& events)
    {
        fs::create_directories(dir);
        chl::RecoveredState recovered;
        recovered.segments = {segment};
        std::string bytes = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, segment, 1, kEndpoint});
        for(auto const& event: events)
        {
            chl::StoryId const story_id = event.getStoryId();
            bytes += wal::encodeNames(wal::NamesRecord{story_id, kChronicle, storyName(story_id)});
            wal::EventFrame frame = wal::encodeEvent(event);
            bytes.append(frame.frame_header.data(), frame.frame_header.size());
            bytes += frame.head;
            bytes.append(frame.record.data(), frame.record.size());
            bytes += frame.tail;

            chl::RecoveredStory& story = recovered.stories[story_id];
            story.wal_story_id = story_id;
            story.has_names = true;
            story.chronicle = kChronicle;
            story.story = storyName(story_id);
            story.events[identityOf(event)] = event;
            story.copies[identityOf(event)].push_back({segment, frame.size()});
        }
        bytes.resize(conf.segment_bytes, '\0');
        std::ofstream(fs::path(dir) / wal::segmentFileName(segment), std::ios::binary) << bytes;
        return recovered;
    }

    std::vector<uint64_t> segmentNumbers()
    {
        std::vector<uint64_t> numbers;
        for(auto const& entry: fs::directory_iterator(dir))
        {
            if(auto number = wal::parseSegmentFileName(entry.path().filename().string()))
            {
                numbers.push_back(*number);
            }
        }
        std::sort(numbers.begin(), numbers.end());
        return numbers;
    }

    std::string readFile(std::string const& name)
    {
        std::ifstream in(fs::path(dir) / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool exists(uint64_t segment) { return fs::exists(fs::path(dir) / wal::segmentFileName(segment)); }

    uint64_t fileSize(uint64_t segment) { return fs::file_size(fs::path(dir) / wal::segmentFileName(segment)); }

    // Every whole record in every segment file, in log order. Reads what has
    // been written whether or not it has been synced; a write still under way
    // can end the scan early with a bad frame, which a polling caller reads
    // again.
    std::vector<OnDisk> records()
    {
        std::vector<OnDisk> found;
        for(uint64_t segment: segmentNumbers())
        {
            std::string bytes = readFile(wal::segmentFileName(segment));
            wal::ScanResult scan = wal::scanSegment(bytes);
            for(auto const& record: scan.records)
            {
                found.push_back(OnDisk{segment, record.type, std::string(record.body)});
            }
        }
        return found;
    }

    static bool decodeEvent(OnDisk const& record, chl::LogEvent& event)
    {
        return record.type == static_cast<uint8_t>(wal::RecordType::Event) &&
               chl::decodeLogEvent(record.body.data() + 1, record.body.size() - 1, event) == chl::CodecStatus::Ok;
    }

    // E records of the event's identity, in any segment
    std::size_t copiesOnDisk(chl::LogEvent const& wanted)
    {
        std::size_t copies = 0;
        for(auto const& record: records())
        {
            chl::LogEvent event;
            if(decodeEvent(record, event) && event.getStoryId() == wanted.getStoryId() &&
               identityOf(event) == identityOf(wanted))
            {
                ++copies;
            }
        }
        return copies;
    }

    // segment of the first E record of the event's identity, 0 when there is none
    uint64_t segmentOf(chl::LogEvent const& wanted)
    {
        for(auto const& record: records())
        {
            chl::LogEvent event;
            if(decodeEvent(record, event) && event.getStoryId() == wanted.getStoryId() &&
               identityOf(event) == identityOf(wanted))
            {
                return record.segment;
            }
        }
        return 0;
    }

    std::size_t droppedRecordsOnDisk(chl::StoryId story)
    {
        std::size_t count = 0;
        for(auto const& record: records())
        {
            wal::DroppedRecord dropped{};
            if(record.type == static_cast<uint8_t>(wal::RecordType::Dropped) &&
               wal::decodeDropped(record.body, dropped) == chl::CL_SUCCESS && dropped.story_id == story)
            {
                ++count;
            }
        }
        return count;
    }

    static chl::StoryChunk chunkOf(chl::StoryId story, std::vector<chl::LogEvent> const& events)
    {
        chl::StoryChunk chunk(kChronicle, storyName(story), story, 0, UINT64_MAX);
        for(auto const& event: events) { EXPECT_EQ(chunk.insertEvent(event), 1); }
        return chunk;
    }

    // declared first: Argobots outlives the WAL and the pools
    tl::abt argobots;
    std::string root;
    std::string dir;
    chl::WalConf conf;
    chl::FaultInjectingWalFileSink* sink = nullptr; // owned by wal
    RenameFailingSink* renameSink = nullptr;        // owned by wal, when makeWal was asked for it
    std::unique_ptr<chl::KeeperWal> wal;
};

} // namespace

// Port of WalJournal.WindowedGroupReportsDurableOnlyAfterSyncReturns. Every
// request waits while the sync is held, more requests than the pool has
// streams pile into one group, and each is answered only once the sync
// returned.
TEST_F(KeeperWalTest, GroupReportsDurableOnlyAfterSyncReturns)
{
    startWal();
    sink->holdSyncs();
    uint64_t const syncs_before = sink->syncCount();

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 32; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }

        // all 32 are waiting: a handler blocking its stream would cap this at 4
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 32; }));
        ASSERT_TRUE(waitFor([&]() { return sink->syncCount() > syncs_before; }));
        std::this_thread::sleep_for(100ms);
        for(auto const& request: requests) { EXPECT_FALSE(request.done.load()); }
        EXPECT_EQ(wal->stats().events, 0u);
        for(auto const& request: requests) { EXPECT_FALSE(wal->isDurable(kStory, identityOf(request.event))); }

        sink->releaseSyncs();
        ASSERT_TRUE(waitFor(
                [&]()
                {
                    for(auto const& request: requests)
                    {
                        if(!request.done.load())
                        {
                            return false;
                        }
                    }
                    return true;
                }));
    }
    for(auto const& request: requests)
    {
        EXPECT_EQ(request.status.load(), chl::CL_SUCCESS);
        EXPECT_TRUE(wal->isDurable(kStory, identityOf(request.event)));
        EXPECT_EQ(copiesOnDisk(request.event), 1u);
    }
    chl::WalStats stats = wal->stats();
    EXPECT_EQ(stats.events, 32u);
    EXPECT_GT(stats.max_events_per_group, 4u);
    EXPECT_LT(stats.groups, 32u);
    EXPECT_EQ(stats.pending_events, 32u);
    EXPECT_EQ(stats.queued_events, 0u);
}

// A nonzero window holds the first request back for requests that follow it.
TEST_F(KeeperWalTest, GroupCommitWindowGathersLaterRequests)
{
    conf.group_commit_window_us = chl::WalConf::MAX_GROUP_COMMIT_WINDOW_US;
    startWal();

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 8; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_SUCCESS); }
    EXPECT_GT(wal->stats().max_events_per_group, 1u);
    EXPECT_EQ(wal->stats().events, 8u);
}

// Port of JournalContract.FsyncFailureFailsTheGroup.
TEST_F(KeeperWalTest, FsyncFailureFailsTheGroup)
{
    startWal();
    sink->holdSyncs();
    sink->failSyncsWith(EIO);

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 3; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 3; }));
        sink->releaseSyncs();
    }
    for(auto const& request: requests)
    {
        EXPECT_EQ(request.status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);
        EXPECT_FALSE(wal->isDurable(kStory, identityOf(request.event)));
    }
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(wal->stats().pending_events, 0u);
    EXPECT_EQ(wal->stats().events, 0u);

    // the latch holds after the device recovers: nothing more is written
    sink->failSyncsWith(0);
    chl::LogEvent const later = makeEvent(kStory, 2000);
    EXPECT_EQ(append(later), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(copiesOnDisk(later), 0u);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
}

TEST_F(KeeperWalTest, WriteEnospcFailsTheGroup)
{
    startWal();
    sink->failWritesWith(ENOSPC);
    uint64_t const syncs_before = sink->syncCount();

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 3; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor(
                [&]()
                {
                    for(auto const& request: requests)
                    {
                        if(!request.done.load())
                        {
                            return false;
                        }
                    }
                    return true;
                }));
    }
    for(auto const& request: requests)
    {
        EXPECT_EQ(request.status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);
        EXPECT_FALSE(wal->isDurable(kStory, identityOf(request.event)));
        EXPECT_EQ(copiesOnDisk(request.event), 0u);
    }
    // a failed write is never followed by a sync that could pass for success
    EXPECT_EQ(sink->syncCount(), syncs_before);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(wal->stats().pending_events, 0u);

    sink->failWritesWith(0);
    chl::LogEvent const later = makeEvent(kStory, 2000);
    EXPECT_EQ(append(later), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(copiesOnDisk(later), 0u);
}

TEST_F(KeeperWalTest, BelowReserveIsFullAndRecovers)
{
    conf.reserve_min_bytes = 4 * kMiB;
    conf.reserve_percent = 10;
    startWal();
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);

    // 10 % of 100 MiB is above the 4 MiB minimum
    sink->setFreeSpace(9 * kMiB, 100 * kMiB);
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Full; }, 2s));
    sink->setFreeSpace(11 * kMiB, 100 * kMiB);
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Ok; }, 2s));

    // on a small file system the minimum decides
    sink->setFreeSpace(3 * kMiB, 10 * kMiB);
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Full; }, 2s));
    sink->setFreeSpace(5 * kMiB, 10 * kMiB);
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Ok; }, 2s));

    EXPECT_EQ(append(makeEvent(kStory, 1000)), chl::CL_SUCCESS);
}

// Each segment carries the names of every story with an event in it, once and
// before the story's first event, so deleting an older segment never leaves
// an event without its names.
TEST_F(KeeperWalTest, NamesRecordWrittenOncePerStoryPerSegment)
{
    startWal();
    std::vector<chl::LogEvent> events;
    for(uint64_t i = 0; i < 24; ++i)
    {
        events.push_back(makeEvent((i % 2 == 0) ? kStory : kOtherStory, 1000 + i, 150 * 1024));
        ASSERT_EQ(append(events.back()), chl::CL_SUCCESS);
    }

    std::map<uint64_t, std::map<chl::StoryId, int>> names_per_segment;
    std::set<uint64_t> segments_with_events;
    for(auto const& record: records())
    {
        if(record.type == static_cast<uint8_t>(wal::RecordType::Names))
        {
            wal::NamesRecord names{};
            ASSERT_EQ(wal::decodeNames(record.body, names), chl::CL_SUCCESS);
            EXPECT_EQ(names.chronicle, kChronicle);
            EXPECT_EQ(names.story, storyName(names.story_id));
            ++names_per_segment[record.segment][names.story_id];
        }
        chl::LogEvent event;
        if(decodeEvent(record, event))
        {
            segments_with_events.insert(record.segment);
            EXPECT_EQ(names_per_segment[record.segment][event.getStoryId()], 1)
                    << "event at " << event.time() << " in segment " << record.segment << " has no N before it";
        }
    }
    EXPECT_GE(segments_with_events.size(), 3u) << "the test needs rotations";
    for(uint64_t segment: segments_with_events)
    {
        EXPECT_EQ(names_per_segment[segment][kStory], 1) << "segment " << segment;
        EXPECT_EQ(names_per_segment[segment][kOtherStory], 1) << "segment " << segment;
    }
    for(auto const& event: events) { EXPECT_EQ(copiesOnDisk(event), 1u); }
}

// A record larger than group_commit_max_bytes and than a segment is not
// refused and does not stall: it goes in a group of its own, in a segment of
// its own size, and the requests around it still commit.
TEST_F(KeeperWalTest, OversizedRecordCommitsAlone)
{
    conf.segment_bytes = 4 * kMiB;
    conf.group_commit_max_bytes = 4 * kMiB;
    startWal();
    sink->holdSyncs();

    std::deque<Append> requests;
    requests.emplace_back().event = makeEvent(kStory, 1000);
    requests.emplace_back().event = makeEvent(kStory, 1001, 8 * kMiB);
    requests.emplace_back().event = makeEvent(kStory, 1002);
    {
        UltPool pool;
        // queued one after the other, behind the first one's held sync
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return segmentOf(requests[0].event) != 0; }));
        appendOn(pool, requests[1]);
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 2; }));
        appendOn(pool, requests[2]);
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 3; }));
        sink->releaseSyncs();
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_SUCCESS); }

    uint64_t const big = segmentOf(requests[1].event);
    ASSERT_NE(big, 0u);
    EXPECT_LT(segmentOf(requests[0].event), big);
    EXPECT_GT(segmentOf(requests[2].event), big);
    EXPECT_GE(fileSize(big), 8 * kMiB);
    std::size_t events_in_big = 0;
    for(auto const& record: records())
    {
        if(record.segment == big && record.type == static_cast<uint8_t>(wal::RecordType::Event))
        {
            ++events_in_big;
        }
    }
    EXPECT_EQ(events_in_big, 1u);
    EXPECT_EQ(wal->stats().events, 3u);
}

TEST_F(KeeperWalTest, RecordLargerThanASegmentGetsASegmentOfItsSize)
{
    conf.segment_bytes = kMiB;
    startWal();
    chl::LogEvent const before = makeEvent(kStory, 1000);
    chl::LogEvent const big = makeEvent(kStory, 1001, 3 * kMiB);
    chl::LogEvent const after = makeEvent(kStory, 1002);
    EXPECT_EQ(append(before), chl::CL_SUCCESS);
    EXPECT_EQ(append(big), chl::CL_SUCCESS);
    EXPECT_EQ(append(after), chl::CL_SUCCESS);

    uint64_t const segment = segmentOf(big);
    EXPECT_GT(segment, segmentOf(before));
    EXPECT_GE(fileSize(segment), 3 * kMiB);
    EXPECT_GT(segmentOf(after), segment);
    EXPECT_EQ(copiesOnDisk(big), 1u);
}

// A retry that arrives while the first attempt waits for its sync waits for
// the same group: one copy on disk, and the first attempt's answer.
TEST_F(KeeperWalTest, RetryJoinsInFlightAppend)
{
    startWal();
    sink->holdSyncs();

    std::deque<Append> requests;
    chl::LogEvent const event = makeEvent(kStory, 1000);
    requests.emplace_back().event = event;
    requests.emplace_back().event = event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        // written and in its held sync
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(event) == 1; }));
        appendOn(pool, requests[1]);
        std::this_thread::sleep_for(100ms);
        EXPECT_FALSE(requests[1].done.load());
        EXPECT_EQ(wal->stats().queued_events, 1u);
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_SUCCESS);
    EXPECT_EQ(requests[1].status.load(), chl::CL_SUCCESS);
    EXPECT_EQ(copiesOnDisk(event), 1u);
    EXPECT_EQ(wal->stats().events, 1u);
    EXPECT_EQ(wal->stats().pending_events, 1u);
}

// The same while the first attempt is still queued behind another group.
TEST_F(KeeperWalTest, RetryJoinsQueuedAppend)
{
    startWal();
    sink->holdSyncs();

    std::deque<Append> requests;
    chl::LogEvent const blocker = makeEvent(kOtherStory, 900);
    chl::LogEvent const event = makeEvent(kStory, 1000);
    requests.emplace_back().event = blocker;
    requests.emplace_back().event = event;
    requests.emplace_back().event = event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(blocker) == 1; }));
        appendOn(pool, requests[1]);
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 2; }));
        appendOn(pool, requests[2]);
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(wal->stats().queued_events, 2u);
        EXPECT_FALSE(requests[2].done.load());
        sink->releaseSyncs();
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_SUCCESS); }
    EXPECT_EQ(copiesOnDisk(event), 1u);
    EXPECT_EQ(wal->stats().events, 2u);
}

// A retry that joined an attempt whose group failed gets that failure, not a
// success and not a second write.
TEST_F(KeeperWalTest, RetryJoinsAFailedAppendAndGetsItsFailure)
{
    startWal();
    sink->holdSyncs();
    sink->failSyncsWith(EIO);

    std::deque<Append> requests;
    chl::LogEvent const event = makeEvent(kStory, 1000);
    requests.emplace_back().event = event;
    requests.emplace_back().event = event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(event) == 1; }));
        appendOn(pool, requests[1]);
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(requests[1].done.load());
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(requests[1].status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(event)));
    EXPECT_EQ(copiesOnDisk(event), 1u); // written, never synced, never counted
}

// A retry of an event already durable gets CL_SUCCESS without a second copy,
// even once the WAL has failed or stopped: the event is on disk.
TEST_F(KeeperWalTest, RetryOfADurableEventIsNotWrittenAgain)
{
    startWal();
    chl::LogEvent const event = makeEvent(kStory, 1000);
    ASSERT_EQ(append(event), chl::CL_SUCCESS);
    EXPECT_EQ(append(event), chl::CL_SUCCESS);
    EXPECT_EQ(copiesOnDisk(event), 1u);
    EXPECT_EQ(wal->stats().events, 1u);

    // fail the WAL with another event
    sink->failSyncsWith(EIO);
    EXPECT_EQ(append(makeEvent(kStory, 2000)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(append(event), chl::CL_SUCCESS);
    EXPECT_EQ(copiesOnDisk(event), 1u);

    // once released, a late retry is a new copy (at-least-once), here refused
    wal->eventDiscarded(kStory, identityOf(event));
    EXPECT_EQ(append(event), chl::CL_ERR_DURABILITY_UNAVAILABLE);
}

TEST_F(KeeperWalTest, IsDurableAfterSyncNotBefore)
{
    startWal();
    chl::LogEvent const event = makeEvent(kStory, 1000);
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(event)));

    sink->holdSyncs();
    std::deque<Append> requests;
    requests.emplace_back().event = event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(event) == 1; }));
        // written but not synced
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(wal->isDurable(kStory, identityOf(event)));
        EXPECT_FALSE(requests[0].done.load());
        sink->releaseSyncs();
        ASSERT_TRUE(waitFor([&]() { return requests[0].done.load(); }));
        // pending before the answer is set, so a retry after the answer is never written again
        EXPECT_TRUE(wal->isDurable(kStory, identityOf(event)));
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_SUCCESS);
    EXPECT_FALSE(wal->isDurable(kOtherStory, identityOf(event)));
}

// A segment goes once nothing in it is pending, oldest first, never the
// active one.
TEST_F(KeeperWalTest, ChunkWrittenReleasesAndDeletesOldSegments)
{
    startWal();
    // three 300 KiB events fill a 1 MiB segment
    std::vector<chl::LogEvent> events;
    for(uint64_t i = 0; i < 7; ++i)
    {
        events.push_back(makeEvent(kStory, 1000 + i, 300 * 1024));
        ASSERT_EQ(append(events.back()), chl::CL_SUCCESS);
    }
    uint64_t const first = segmentOf(events[0]);
    uint64_t const second = segmentOf(events[3]);
    uint64_t const active = segmentOf(events[6]);
    ASSERT_EQ(segmentOf(events[2]), first);
    ASSERT_EQ(segmentOf(events[5]), second);
    ASSERT_LT(first, second);
    ASSERT_LT(second, active);

    // the same identities under another story release nothing
    std::vector<chl::LogEvent> other;
    for(auto const& event: events)
    {
        other.push_back(event);
        other.back().storyId = kOtherStory;
    }
    wal->chunkWritten(kOtherStory, chunkOf(kOtherStory, other));
    EXPECT_EQ(wal->stats().pending_events, 7u);

    // the second segment is released first: the first still holds it back
    wal->chunkWritten(kStory, chunkOf(kStory, {events[3], events[4], events[5]}));
    EXPECT_EQ(wal->stats().pending_events, 4u);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(exists(first));
    EXPECT_TRUE(exists(second));

    wal->chunkWritten(kStory, chunkOf(kStory, {events[0], events[1], events[2]}));
    ASSERT_TRUE(waitFor([&]() { return !exists(first) && !exists(second); }, 3s));
    EXPECT_TRUE(exists(active));
    EXPECT_EQ(wal->stats().pending_events, 1u);
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(events[6])));

    // the last one: the active segment stays
    wal->chunkWritten(kStory, chunkOf(kStory, {events[6]}));
    EXPECT_EQ(wal->stats().pending_events, 0u);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(exists(active));
}

TEST_F(KeeperWalTest, DiscardedChunkAndEventAreReleased)
{
    startWal();
    chl::LogEvent const in_chunk = makeEvent(kStory, 1000);
    chl::LogEvent const alone = makeEvent(kStory, 1001);
    chl::LogEvent const kept = makeEvent(kStory, 1002);
    for(auto const& event: {in_chunk, alone, kept}) { ASSERT_EQ(append(event), chl::CL_SUCCESS); }

    wal->chunkDiscarded(kStory, chunkOf(kStory, {in_chunk}));
    wal->eventDiscarded(kStory, identityOf(alone));
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(in_chunk)));
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(alone)));
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(kept)));
    EXPECT_EQ(wal->stats().pending_events, 1u);
}

// storyDropped never waits; the story's copies stay pending until the D
// record syncs, and events written after the D stay pending.
TEST_F(KeeperWalTest, StoryDroppedReleasesAfterTheDCommits)
{
    startWal();
    chl::LogEvent const first = makeEvent(kStory, 1000);
    chl::LogEvent const second = makeEvent(kStory, 1001);
    chl::LogEvent const other = makeEvent(kOtherStory, 1002);
    for(auto const& event: {first, second, other}) { ASSERT_EQ(append(event), chl::CL_SUCCESS); }

    sink->holdSyncs();
    wal->storyDropped(kStory); // returns with the sync held
    ASSERT_TRUE(waitFor([&]() { return droppedRecordsOnDisk(kStory) == 1; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(first)));
    EXPECT_EQ(wal->stats().pending_events, 3u);

    // the story re-created under the same id: written after the D
    std::deque<Append> requests;
    requests.emplace_back().event = makeEvent(kStory, 1003);
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 1; }));
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&]() { return !wal->isDurable(kStory, identityOf(first)); }));
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(second)));
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(requests[0].event)));
    EXPECT_TRUE(wal->isDurable(kOtherStory, identityOf(other)));
    EXPECT_EQ(wal->stats().pending_events, 2u);
    EXPECT_EQ(droppedRecordsOnDisk(kStory), 1u);
}

// The retention store calls the observer with its tailMutex held, so none of
// these may wait for a sync in progress; neither may the lookups a durable
// request makes before it queues.
TEST_F(KeeperWalTest, ObserverCallsDoNotWaitForASync)
{
    startWal();
    chl::LogEvent const committed = makeEvent(kStory, 900);
    ASSERT_EQ(append(committed), chl::CL_SUCCESS);
    sink->holdSyncs();

    std::deque<Append> requests;
    requests.emplace_back().event = makeEvent(kStory, 1000);
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(requests[0].event) == 1; })); // in its held sync

        auto const started = std::chrono::steady_clock::now();
        wal->chunkWritten(kStory, chunkOf(kStory, {committed}));
        wal->chunkDiscarded(kOtherStory, chunkOf(kOtherStory, {makeEvent(kOtherStory, 900)}));
        wal->eventDiscarded(kStory, chl::EventSequence{1, 1, 1});
        wal->storyDropped(kOtherStory);
        EXPECT_FALSE(wal->isDurable(kStory, identityOf(committed)));
        EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);
        EXPECT_EQ(wal->stats().queued_events, 1u);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
        EXPECT_FALSE(requests[0].done.load());
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_SUCCESS);
}

// stop() with requests queued behind a held sync returns once the sync
// does, and every waiter has an answer; after it, a write is refused at once.
TEST_F(KeeperWalTest, StopAnswersEveryWaiter)
{
    startWal();
    sink->holdSyncs();

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 16; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 16; }));

        std::thread releaser(
                [this]()
                {
                    std::this_thread::sleep_for(200ms);
                    sink->releaseSyncs();
                });
        wal->stop();
        releaser.join();
        for(auto const& request: requests) { EXPECT_TRUE(request.done.load()); }
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_SUCCESS); }
    EXPECT_EQ(wal->stats().events, 16u);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(append(makeEvent(kStory, 2000)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    wal->stop(); // a second stop does nothing
}

TEST_F(KeeperWalTest, StopAnswersEveryWaiterWhenTheSyncFails)
{
    startWal();
    sink->holdSyncs();
    sink->failSyncsWith(EIO);

    std::deque<Append> requests;
    for(uint64_t i = 0; i < 16; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    {
        UltPool pool;
        for(auto& request: requests) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 16; }));
        std::thread releaser(
                [this]()
                {
                    std::this_thread::sleep_for(200ms);
                    sink->releaseSyncs();
                });
        wal->stop();
        releaser.join();
        for(auto const& request: requests) { EXPECT_TRUE(request.done.load()); }
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE); }
    EXPECT_EQ(wal->stats().pending_events, 0u);
}

TEST_F(KeeperWalTest, AppendBeforeStartOrAfterStopIsUnavailable)
{
    makeWal();
    chl::LogEvent const event = makeEvent(kStory, 1000);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(append(event), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    wal->storyDropped(kStory); // ignored, nothing to write it with

    ASSERT_EQ(wal->start(chl::RecoveredState{}, {}), chl::CL_SUCCESS);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);
    EXPECT_EQ(append(event), chl::CL_SUCCESS);
    wal->stop();
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(append(makeEvent(kStory, 1001)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(wal->start(chl::RecoveredState{}, {}), chl::CL_ERR_UNKNOWN);
}

// After a restart the recovered copies are pending under the story's live id,
// so the grapher's confirmation of the replayed chunk releases them, and a
// header-only spare left by the crash becomes the active segment.
TEST_F(KeeperWalTest, RecoveredCopiesArePendingUnderTheLiveId)
{
    constexpr chl::StoryId kWalId = 111;
    constexpr chl::StoryId kLiveId = 222;
    chl::LogEvent recovered_event = makeEvent(kWalId, 1000);
    fs::create_directories(dir);
    {
        // segment 1 holds the event, segment 2 only its header
        std::string one = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, 1, 1, kEndpoint});
        one += wal::encodeNames(wal::NamesRecord{kWalId, kChronicle, storyName(kWalId)});
        wal::EventFrame frame = wal::encodeEvent(recovered_event);
        one.append(frame.frame_header.data(), frame.frame_header.size());
        one += frame.head;
        one.append(frame.record.data(), frame.record.size());
        one += frame.tail;
        one.resize(kMiB, '\0');
        std::ofstream(fs::path(dir) / wal::segmentFileName(1), std::ios::binary) << one;
        std::string two = wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, 2, 1, kEndpoint});
        two.resize(kMiB, '\0');
        std::ofstream(fs::path(dir) / wal::segmentFileName(2), std::ios::binary) << two;
    }

    chl::RecoveredState recovered;
    recovered.segments = {1, 2};
    recovered.reusable_spare = 2;
    chl::RecoveredStory& story = recovered.stories[kWalId];
    story.wal_story_id = kWalId;
    story.has_names = true;
    story.chronicle = kChronicle;
    story.story = storyName(kWalId);
    story.events[identityOf(recovered_event)] = recovered_event;
    story.copies[identityOf(recovered_event)].push_back({1, wal::encodeEvent(recovered_event).size()});

    makeWal();
    // as recovery does: through the WAL's own sink and segment manager
    ASSERT_EQ(wal->fileSink().openDirectory(dir), 0);
    ASSERT_EQ(wal->segmentManager().open(), 0);
    ASSERT_EQ(wal->start(recovered, [](chl::RecoveredStory const&) { return kLiveId; }), chl::CL_SUCCESS);

    EXPECT_TRUE(wal->isDurable(kLiveId, identityOf(recovered_event)));
    EXPECT_FALSE(wal->isDurable(kWalId, identityOf(recovered_event)));
    EXPECT_EQ(wal->stats().pending_events, 1u);

    // new writes go into the adopted spare
    chl::LogEvent const fresh = makeEvent(kLiveId, 2000);
    ASSERT_EQ(append(fresh), chl::CL_SUCCESS);
    EXPECT_EQ(segmentOf(fresh), 2u);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(exists(1));

    chl::LogEvent live_copy = recovered_event;
    live_copy.storyId = kLiveId;
    wal->chunkWritten(kLiveId, chunkOf(kLiveId, {live_copy}));
    EXPECT_FALSE(wal->isDurable(kLiveId, identityOf(recovered_event)));
    ASSERT_TRUE(waitFor([&]() { return !exists(1); }, 3s));
    EXPECT_TRUE(exists(2));
}

// When everything in the active segment has been released, the committer
// moves to a new segment so the old one can be deleted, but not before the
// interval and not while an event in it is pending.
TEST_F(KeeperWalTest, ReleasedActiveSegmentRotatesAndIsDeleted)
{
    startWal(200ms);
    chl::LogEvent const event = makeEvent(kStory, 1000);
    ASSERT_EQ(append(event), chl::CL_SUCCESS);
    uint64_t const segment = segmentOf(event);
    ASSERT_NE(segment, 0u);

    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(exists(segment)); // still pending

    wal->eventDiscarded(kStory, identityOf(event));
    ASSERT_TRUE(waitFor([&]() { return !exists(segment); }, 3s));
    // counted once the directory sync after the removal returns
    EXPECT_TRUE(waitFor([&]() { return wal->stats().segments == 1; }));

    // an active segment with nothing written is left alone
    ASSERT_TRUE(waitFor([this]() { return segmentNumbers().size() == 2; })); // the active one and the next spare
    std::vector<uint64_t> const before = segmentNumbers();
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(wal->stats().segments, 1u);
    EXPECT_EQ(segmentNumbers(), before);

    chl::LogEvent const later = makeEvent(kStory, 2000);
    ASSERT_EQ(append(later), chl::CL_SUCCESS);
    EXPECT_GT(segmentOf(later), segment);
}

// Many writers across many rotations: every write succeeds, each event is on
// disk exactly once after its story's N record, and releasing everything
// leaves only the active segment.
TEST_F(KeeperWalTest, ManyWritersAcrossRotationsWriteEachEventOnce)
{
    startWal();
    constexpr int kWriters = 64;
    constexpr int kPerWriter = 10;
    std::deque<chl::LogEvent> events;
    for(int writer = 0; writer < kWriters; ++writer)
    {
        for(int i = 0; i < kPerWriter; ++i)
        {
            chl::LogEvent event = makeEvent(100 + writer % 4, 1000 + i, 20 * 1024, writer);
            events.push_back(event);
        }
    }
    std::atomic<int> failures{0};
    {
        UltPool pool;
        for(int writer = 0; writer < kWriters; ++writer)
        {
            pool.run(
                    [&, writer]()
                    {
                        for(int i = 0; i < kPerWriter; ++i)
                        {
                            if(append(events[writer * kPerWriter + i]) != chl::CL_SUCCESS)
                            {
                                ++failures;
                            }
                        }
                    });
        }
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(wal->stats().events, static_cast<uint64_t>(kWriters * kPerWriter));
    EXPECT_EQ(wal->stats().pending_events, static_cast<uint64_t>(kWriters * kPerWriter));

    std::map<std::pair<chl::StoryId, chl::EventSequence>, int> copies;
    std::map<uint64_t, std::set<chl::StoryId>> named;
    std::set<uint64_t> segments_with_events;
    for(auto const& record: records())
    {
        wal::NamesRecord names{};
        if(record.type == static_cast<uint8_t>(wal::RecordType::Names) &&
           wal::decodeNames(record.body, names) == chl::CL_SUCCESS)
        {
            named[record.segment].insert(names.story_id);
        }
        chl::LogEvent event;
        if(decodeEvent(record, event))
        {
            segments_with_events.insert(record.segment);
            EXPECT_EQ(named[record.segment].count(event.getStoryId()), 1u);
            ++copies[{event.getStoryId(), identityOf(event)}];
        }
    }
    EXPECT_GE(segments_with_events.size(), 5u);
    EXPECT_EQ(copies.size(), events.size());
    for(auto const& event: events) { EXPECT_EQ((copies[{event.getStoryId(), identityOf(event)}]), 1); }

    std::map<chl::StoryId, std::vector<chl::LogEvent>> by_story;
    for(auto const& event: events) { by_story[event.getStoryId()].push_back(event); }
    for(auto const& [story, story_events]: by_story) { wal->chunkWritten(story, chunkOf(story, story_events)); }
    EXPECT_EQ(wal->stats().pending_events, 0u);
    EXPECT_EQ(wal->stats().pending_bytes, 0u);
    ASSERT_TRUE(waitFor([&]() { return wal->stats().segments == 1; }, 3s));
}

// A2: a retry is answered by its first attempt before any other check. With
// the reserve full, so W14 would refuse a new write, a lookup of an attempt in
// its group's sync and of one queued behind it waits for them and gets their
// CL_SUCCESS, instead of reporting the identity unknown.
TEST_F(KeeperWalTest, LookupWaitsForAQueuedAttemptWhileTheReserveIsFull)
{
    conf.reserve_min_bytes = 4 * kMiB;
    startWal();
    // full first: the free space is sampled by the committer, which a held
    // sync stops
    sink->setFreeSpace(kMiB, 100 * kMiB);
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Full; }, 2s));
    sink->holdSyncs();

    chl::LogEvent const blocker = makeEvent(kOtherStory, 900);
    chl::LogEvent const event = makeEvent(kStory, 1000);
    std::deque<Append> requests;
    requests.emplace_back().event = blocker;
    requests.emplace_back().event = event;
    std::deque<Lookup> lookups;
    lookups.emplace_back().event = blocker; // in its group's sync
    lookups.emplace_back().event = event;   // queued behind it
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(blocker) == 1; }));
        appendOn(pool, requests[1]);
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 2; }));

        for(auto& lookup: lookups) { lookupOn(pool, lookup); }
        std::this_thread::sleep_for(100ms);
        for(auto const& lookup: lookups) { EXPECT_FALSE(lookup.done.load()); }
        EXPECT_EQ(wal->admit(), chl::WalAdmit::Full);
        EXPECT_EQ(wal->stats().queued_events, 2u);
        // an identity the WAL does not hold is answered at once
        EXPECT_EQ(wal->lookupDurable(kStory, chl::EventSequence{5, 1, 0}), std::nullopt);
        sink->releaseSyncs();
    }
    for(auto const& request: requests) { EXPECT_EQ(request.status.load(), chl::CL_SUCCESS); }
    for(auto const& lookup: lookups) { EXPECT_EQ(lookup.answer, std::optional<int>(chl::CL_SUCCESS)); }
    EXPECT_EQ(copiesOnDisk(event), 1u);
    EXPECT_EQ(wal->stats().events, 2u);
    // durable now: answered at once, the reserve still full
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::optional<int>(chl::CL_SUCCESS));
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Full);
}

// A lookup that joined an attempt whose group failed gets that failure; once
// answered, the identity is unknown to the WAL.
TEST_F(KeeperWalTest, LookupOfAFailedAttemptGetsItsFailure)
{
    startWal();
    sink->holdSyncs();
    sink->failSyncsWith(EIO);

    chl::LogEvent const event = makeEvent(kStory, 1000);
    std::deque<Append> requests;
    requests.emplace_back().event = event;
    std::deque<Lookup> lookups;
    lookups.emplace_back().event = event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(event) == 1; }));
        lookupOn(pool, lookups[0]);
        std::this_thread::sleep_for(50ms);
        EXPECT_FALSE(lookups[0].done.load());
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(lookups[0].answer, std::optional<int>(chl::CL_ERR_DURABILITY_UNAVAILABLE));
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::nullopt);
}

// A durable event is answered CL_SUCCESS by the lookup whether the WAL runs,
// has failed or has stopped; an unknown or released one is std::nullopt.
TEST_F(KeeperWalTest, LookupAnswersADurableEventInEveryState)
{
    makeWal();
    chl::LogEvent const event = makeEvent(kStory, 1000);
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::nullopt); // not started

    ASSERT_EQ(wal->start(chl::RecoveredState{}, {}), chl::CL_SUCCESS);
    ASSERT_EQ(append(event), chl::CL_SUCCESS);
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::optional<int>(chl::CL_SUCCESS));
    EXPECT_EQ(wal->lookupDurable(kOtherStory, identityOf(event)), std::nullopt);

    sink->failSyncsWith(EIO);
    EXPECT_EQ(append(makeEvent(kStory, 2000)), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    ASSERT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::optional<int>(chl::CL_SUCCESS));

    wal->stop();
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::optional<int>(chl::CL_SUCCESS));
    wal->eventDiscarded(kStory, identityOf(event));
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(event)), std::nullopt);
}

// A group of more than one batch (1024 events) is registered across the batch
// boundary: every event durable, and a retry of an event in the second batch
// joins it.
TEST_F(KeeperWalTest, GroupLargerThanABatchRegistersEveryEvent)
{
    constexpr uint64_t kEvents = 1500;
    startWal();
    sink->holdSyncs();

    chl::LogEvent const blocker = makeEvent(kOtherStory, 900);
    std::deque<Append> requests;
    requests.emplace_back().event = blocker;
    for(uint64_t i = 0; i < kEvents; ++i) { requests.emplace_back().event = makeEvent(kStory, 1000 + i); }
    std::deque<Append> retry;
    retry.emplace_back().event = requests[1400].event;
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(blocker) == 1; }));
        for(std::size_t i = 1; i < requests.size(); ++i) { appendOn(pool, requests[i]); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == kEvents + 1; }, 20s));
        appendOn(pool, retry[0]);
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(wal->stats().queued_events, kEvents + 1);
        sink->releaseSyncs();
    }
    for(auto const& request: requests)
    {
        EXPECT_EQ(request.status.load(), chl::CL_SUCCESS);
        EXPECT_TRUE(wal->isDurable(request.event.getStoryId(), identityOf(request.event)));
    }
    EXPECT_EQ(retry[0].status.load(), chl::CL_SUCCESS);
    EXPECT_EQ(copiesOnDisk(retry[0].event), 1u);
    chl::WalStats const stats = wal->stats();
    EXPECT_EQ(stats.events, kEvents + 1);
    EXPECT_EQ(stats.pending_events, kEvents + 1);
    EXPECT_GE(stats.max_events_per_group, kEvents);
    EXPECT_EQ(stats.queued_events, 0u);
}

// A D record for a story with more pending copies than a batch drops them all
// in batches, together with the story's events written before the D in the
// same group, and keeps the events written after it in that group.
TEST_F(KeeperWalTest, StoryDropAcrossBatchesKeepsTheEventsAfterIt)
{
    startWal();
    std::vector<chl::LogEvent> backlog;
    for(uint64_t i = 0; i < 3200; ++i) { backlog.push_back(makeEvent(kStory, 1000 + i)); }
    ASSERT_TRUE(appendAll(backlog));
    ASSERT_EQ(wal->stats().pending_events, backlog.size());

    sink->holdSyncs();
    chl::LogEvent const blocker = makeEvent(kOtherStory, 900);
    std::deque<Append> requests;
    requests.emplace_back().event = blocker;
    std::deque<Append> before;
    std::deque<Append> after;
    for(uint64_t i = 0; i < 5; ++i)
    {
        before.emplace_back().event = makeEvent(kStory, 5000 + i);
        after.emplace_back().event = makeEvent(kStory, 6000 + i);
    }
    {
        UltPool pool;
        appendOn(pool, requests[0]);
        ASSERT_TRUE(waitFor([&]() { return copiesOnDisk(blocker) == 1; }));
        // queued behind the blocker's held sync, so all in the next group, in this order
        for(auto& request: before) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 6; }));
        wal->storyDropped(kStory);
        for(auto& request: after) { appendOn(pool, request); }
        ASSERT_TRUE(waitFor([&]() { return wal->stats().queued_events == 11; }));
        sink->releaseSyncs();
    }
    EXPECT_EQ(requests[0].status.load(), chl::CL_SUCCESS);
    for(auto const& request: before)
    {
        EXPECT_EQ(request.status.load(), chl::CL_SUCCESS);
        EXPECT_FALSE(wal->isDurable(kStory, identityOf(request.event)));
    }
    for(auto const& request: after)
    {
        EXPECT_EQ(request.status.load(), chl::CL_SUCCESS);
        EXPECT_TRUE(wal->isDurable(kStory, identityOf(request.event)));
    }
    std::size_t still_pending = 0;
    for(auto const& event: backlog) { still_pending += wal->isDurable(kStory, identityOf(event)) ? 1 : 0; }
    EXPECT_EQ(still_pending, 0u);
    EXPECT_TRUE(wal->isDurable(kOtherStory, identityOf(blocker)));
    EXPECT_EQ(wal->stats().pending_events, after.size() + 1);
    EXPECT_EQ(droppedRecordsOnDisk(kStory), 1u);

    // the copies after the D are still released by their chunk
    std::vector<chl::LogEvent> kept;
    for(auto const& request: after) { kept.push_back(request.event); }
    wal->chunkWritten(kStory, chunkOf(kStory, kept));
    EXPECT_EQ(wal->stats().pending_events, 1u);
}

// Chunks with more events than a batch, of a story with more pending events
// than a batch, release exactly their own events: a chunk smaller than the
// backlog, then one larger than what is left that also holds events the WAL
// never had.
TEST_F(KeeperWalTest, ChunkReleaseAcrossBatchesReleasesExactlyItsEvents)
{
    startWal();
    std::vector<chl::LogEvent> backlog;
    for(uint64_t i = 0; i < 3200; ++i) { backlog.push_back(makeEvent(kStory, 1000 + i)); }
    ASSERT_TRUE(appendAll(backlog));
    chl::LogEvent const other = makeEvent(kOtherStory, 1000);
    ASSERT_EQ(append(other), chl::CL_SUCCESS);
    ASSERT_EQ(wal->stats().pending_events, backlog.size() + 1);

    // two of every three events, then the rest
    std::vector<chl::LogEvent> first;
    std::vector<chl::LogEvent> rest;
    for(std::size_t i = 0; i < backlog.size(); ++i) { ((i % 3 != 2) ? first : rest).push_back(backlog[i]); }
    ASSERT_GT(rest.size(), 1024u);
    wal->chunkWritten(kStory, chunkOf(kStory, first));
    EXPECT_EQ(wal->stats().pending_events, rest.size() + 1);
    for(auto const& event: first) { EXPECT_FALSE(wal->isDurable(kStory, identityOf(event))); }
    for(auto const& event: rest) { EXPECT_TRUE(wal->isDurable(kStory, identityOf(event))); }

    std::vector<chl::LogEvent> larger = rest;
    for(uint64_t i = 0; i < 4000; ++i) { larger.push_back(makeEvent(kStory, 100000 + i)); }
    wal->chunkDiscarded(kStory, chunkOf(kStory, larger));
    EXPECT_EQ(wal->stats().pending_events, 1u);
    EXPECT_EQ(wal->stats().pending_bytes, wal::encodeEvent(other).size());
    EXPECT_TRUE(wal->isDurable(kOtherStory, identityOf(other)));
}

// A large chunk release and a large story drop go a batch at a time, and a
// thread waiting for the WAL's mutex, as an RPC stream calling isDurable
// would, gets it between batches: it sees the pending count part of the way
// down. One long hold, or batches that take the mutex straight back (glibc's
// mutex is not fair), would let it in only at the end.
TEST_F(KeeperWalTest, BatchesLetAWaitingThreadIn)
{
    constexpr std::size_t kEvents = 50000;
    conf.segment_bytes = 32 * kMiB;
    startWal();

    // the pending counts strictly between 0 and kEvents a thread polling
    // stats() saw while work ran
    auto seenPartWayDown = [&](std::function<void()> const& work) -> std::size_t
    {
        std::atomic<bool> working{true};
        std::set<uint64_t> seen;
        std::thread prober(
                [&]()
                {
                    while(working.load())
                    {
                        uint64_t const pending = wal->stats().pending_events;
                        if(pending > 1 && pending < kEvents + 1)
                        {
                            seen.insert(pending);
                        }
                        std::this_thread::sleep_for(20us);
                    }
                });
        work();
        working = false;
        prober.join();
        return seen.size();
    };

    chl::LogEvent const other = makeEvent(kOtherStory, 1000);
    ASSERT_EQ(append(other), chl::CL_SUCCESS);
    std::vector<chl::LogEvent> released;
    std::vector<chl::LogEvent> dropped;
    for(uint64_t i = 0; i < kEvents; ++i)
    {
        released.push_back(makeEvent(kStory, 1000 + i));
        dropped.push_back(makeEvent(kStory, 1000000 + i));
    }
    chl::StoryChunk const chunk = chunkOf(kStory, released);

    ASSERT_TRUE(appendAll(released, 512));
    ASSERT_EQ(wal->stats().pending_events, kEvents + 1);
    std::size_t const during_release = seenPartWayDown([&]() { wal->chunkWritten(kStory, chunk); });
    EXPECT_EQ(wal->stats().pending_events, 1u);
    // about 49 batches
    EXPECT_GE(during_release, 5u);

    ASSERT_TRUE(appendAll(dropped, 512));
    ASSERT_EQ(wal->stats().pending_events, kEvents + 1);
    std::size_t const during_drop = seenPartWayDown(
            [&]()
            {
                wal->storyDropped(kStory);
                EXPECT_TRUE(waitFor([&]() { return wal->stats().pending_events == 1; }, 30s));
            });
    EXPECT_GE(during_drop, 5u);
    EXPECT_TRUE(wal->isDurable(kOtherStory, identityOf(other)));
}

// A failed sync of the active segment before an idle rotation latches the WAL
// like a failed group sync, and the segment is not replaced.
TEST_F(KeeperWalTest, IdleRotationSyncFailureLatches)
{
    startWal(200ms);
    chl::LogEvent const event = makeEvent(kStory, 1000);
    ASSERT_EQ(append(event), chl::CL_SUCCESS);
    uint64_t const segment = segmentOf(event);
    ASSERT_NE(segment, 0u);

    sink->failSyncsWith(EIO);
    wal->eventDiscarded(kStory, identityOf(event));
    ASSERT_TRUE(waitFor([&]() { return wal->admit() == chl::WalAdmit::Unavailable; }, 3s));
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(exists(segment));

    sink->failSyncsWith(0);
    chl::LogEvent const later = makeEvent(kStory, 2000);
    EXPECT_EQ(append(later), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(copiesOnDisk(later), 0u);
}

// When only the spare for an idle rotation cannot be made, the synced active
// segment stays in use and the WAL keeps taking durable writes; the rotation
// is tried again an interval later and goes through once spares can be made.
TEST_F(KeeperWalTest, IdleRotationWithoutASpareKeepsTheActiveSegment)
{
    startWal(200ms, true);
    ASSERT_NE(renameSink, nullptr);
    // the ready spare is taken by the first rotation; every later spare fails
    // at its rename
    renameSink->failRenamesWith(EIO);

    chl::LogEvent const first = makeEvent(kStory, 1000);
    ASSERT_EQ(append(first), chl::CL_SUCCESS);
    uint64_t const one = segmentOf(first);
    wal->eventDiscarded(kStory, identityOf(first));
    ASSERT_TRUE(waitFor([&]() { return !exists(one); }, 3s));

    chl::LogEvent const second = makeEvent(kStory, 2000);
    ASSERT_EQ(append(second), chl::CL_SUCCESS);
    uint64_t const two = segmentOf(second);
    ASSERT_GT(two, one);
    uint64_t const renames_before = renameSink->renameCount();
    wal->eventDiscarded(kStory, identityOf(second));
    // the preparer's attempt and the committer's own
    ASSERT_TRUE(waitFor([&]() { return renameSink->renameCount() >= renames_before + 2; }, 3s));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);
    EXPECT_TRUE(exists(two));

    chl::LogEvent const third = makeEvent(kStory, 3000);
    EXPECT_EQ(append(third), chl::CL_SUCCESS);
    EXPECT_EQ(segmentOf(third), two);
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(third)));

    renameSink->failRenamesWith(0);
    wal->eventDiscarded(kStory, identityOf(third));
    ASSERT_TRUE(waitFor([&]() { return !exists(two); }, 3s));
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);
    chl::LogEvent const fourth = makeEvent(kStory, 4000);
    EXPECT_EQ(append(fourth), chl::CL_SUCCESS);
    EXPECT_GT(segmentOf(fourth), two);
}

// When the committer's stream cannot be created, start returns CL_ERR_UNKNOWN
// instead of throwing, leaves the WAL not started with no recovered copy
// pending, refuses a write at once instead of queuing it for a committer that
// does not exist, and can be called again; the second start adds the
// recovered copies once.
TEST_F(KeeperWalTest, StartThatCannotCreateItsCommitterCanBeRetried)
{
    chl::LogEvent const recovered_event = makeEvent(kStory, 1000);
    chl::RecoveredState const recovered = writeRecoveredSegment(1, {recovered_event});
    makeWal();

    int status = chl::CL_SUCCESS;
    {
        NoNewThreads limit;
        if(!limit.refused())
        {
            limit.restore();
            GTEST_SKIP() << "threads are still created under RLIMIT_NPROC 0 (a privileged user)";
        }
        EXPECT_NO_THROW(status = wal->start(recovered, {}));
    }
    ASSERT_EQ(status, chl::CL_ERR_UNKNOWN);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(wal->stats().pending_events, 0u);
    EXPECT_FALSE(wal->isDurable(kStory, identityOf(recovered_event)));
    EXPECT_EQ(wal->lookupDurable(kStory, identityOf(recovered_event)), std::nullopt);
    std::atomic<int> answer{1};
    std::thread writer([&]() { answer = append(makeEvent(kStory, 2000)); });
    if(!waitFor([&]() { return answer.load() != 1; }))
    {
        // left waiting for a committer: the WAL cannot be destroyed under it
        writer.detach();
        std::abort();
    }
    writer.join();
    EXPECT_EQ(answer.load(), chl::CL_ERR_DURABILITY_UNAVAILABLE);

    ASSERT_EQ(wal->start(recovered, {}), chl::CL_SUCCESS);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Ok);
    EXPECT_EQ(wal->stats().pending_events, 1u);
    EXPECT_EQ(wal->stats().pending_bytes, wal::encodeEvent(recovered_event).size());
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(recovered_event)));
    chl::LogEvent const fresh = makeEvent(kStory, 3000);
    EXPECT_EQ(append(fresh), chl::CL_SUCCESS);

    // the recovered copy was its segment's only one: released, the segment goes
    wal->chunkWritten(kStory, chunkOf(kStory, {recovered_event}));
    EXPECT_EQ(wal->stats().pending_events, 1u);
    EXPECT_TRUE(waitFor([&]() { return !exists(1); }, 3s));
    EXPECT_TRUE(wal->isDurable(kStory, identityOf(fresh)));
}

// A start that fails because no spare can be made adds no recovered copy, so
// the start that follows adds each of them once.
TEST_F(KeeperWalTest, StartRetriedAfterAFailedSpareAddsRecoveredCopiesOnce)
{
    chl::LogEvent const recovered_event = makeEvent(kStory, 1000);
    chl::RecoveredState const recovered = writeRecoveredSegment(1, {recovered_event});
    makeWal();

    sink->failWritesWith(ENOSPC); // the spare cannot be zero-filled
    EXPECT_EQ(wal->start(recovered, {}), chl::CL_ERR_UNKNOWN);
    EXPECT_EQ(wal->admit(), chl::WalAdmit::Unavailable);
    EXPECT_EQ(wal->stats().pending_events, 0u);
    EXPECT_EQ(wal->stats().pending_bytes, 0u);

    sink->failWritesWith(0);
    ASSERT_EQ(wal->start(recovered, {}), chl::CL_SUCCESS);
    EXPECT_EQ(wal->stats().pending_events, 1u);
    EXPECT_EQ(wal->stats().pending_bytes, wal::encodeEvent(recovered_event).size());
    wal->chunkWritten(kStory, chunkOf(kStory, {recovered_event}));
    EXPECT_EQ(wal->stats().pending_events, 0u);
    EXPECT_EQ(wal->stats().pending_bytes, 0u);
    EXPECT_TRUE(waitFor([&]() { return !exists(1); }, 3s));
}
