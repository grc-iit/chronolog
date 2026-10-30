// Unit tests for recoverWal, which reads a keeper WAL directory back at
// startup. Recovery must return every event a segment still holds, with every
// copy of it, so the WAL keeps each segment until the grapher confirms what is
// in it. It may zero only the torn tail of the newest segment with data, and
// must refuse, changing no file, on a bad record anywhere else, an unknown
// format or codec version, or an unknown record type. Before it returns it must
// sync every segment and the directory, since the keeper that wrote them may
// have died before its last sync. Runs against a temporary directory through
// the fault-injecting sink.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cerrno>

#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <Crc32c.h>
#include <WalFileSink.h>
#include <WalFormat.h>
#include <WalRecovery.h>
#include <WalReleaseTracker.h>
#include <WalSegmentManager.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t kSegmentBytes = 64u << 10;
std::string const kEndpoint = "ofi+sockets://127.0.0.1:6666";
constexpr chl::StoryId kStory = 0x1111;
constexpr chl::StoryId kOther = 0x2222;

std::string le(uint64_t value, int bytes)
{
    std::string out;
    for(int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF)); }
    return out;
}

// a frame around an arbitrary body, built here rather than by WalFormat
std::string frameOf(std::string const& body)
{
    return le(body.size(), 4) + le(chl::crc32c_extend(0, body.data(), body.size()), 4) + body;
}

std::string frameOf(chl::LogEvent const& event)
{
    wal::EventFrame const frame = wal::encodeEvent(event);
    return std::string(frame.frame_header.data(), frame.frame_header.size()) + frame.head + std::string(frame.record) +
           frame.tail;
}

chl::LogEvent eventOf(chl::StoryId story, uint64_t time, chl::ClientId client = 7, uint32_t index = 0)
{
    return chl::LogEvent(story, time, client, index, "record-" + std::to_string(story) + "-" + std::to_string(time));
}

chl::EventSequence seqOf(chl::LogEvent const& event) { return {event.time(), event.getClientId(), event.index()}; }

using Copies = std::vector<std::pair<uint64_t, uint64_t>>;

// One segment's bytes as the committer leaves them: the H record, then frames.
// Each append returns the frame's offset.
class Segment
{
public:
    explicit Segment(uint64_t number, uint16_t version = wal::kFormatVersion, uint64_t created_ns = 1)
        : number(number)
        , bytes(wal::encodeHeader(wal::HeaderRecord{version, number, created_ns, kEndpoint}))
    {}

    uint64_t names(chl::StoryId story, std::string const& chronicle, std::string const& name)
    {
        return raw(wal::encodeNames(wal::NamesRecord{story, chronicle, name}));
    }

    uint64_t event(chl::LogEvent const& event) { return raw(frameOf(event)); }

    uint64_t dropped(chl::StoryId story) { return raw(wal::encodeDropped(wal::DroppedRecord{story})); }

    uint64_t zeros(std::size_t count) { return raw(std::string(count, '\0')); }

    uint64_t raw(std::string const& frame)
    {
        uint64_t const offset = bytes.size();
        bytes += frame;
        return offset;
    }

    uint64_t end() const { return bytes.size(); }

    uint64_t const number;
    std::string bytes;
};

// Wraps the real sink and logs which file each write and data sync was for, so
// a test can tell which segments recovery made durable and in what order. It
// can also fail the data syncs of one file.
class SyncRecordingSink: public chl::WalFileSink
{
public:
    explicit SyncRecordingSink(std::unique_ptr<chl::WalFileSink> inner)
        : inner(std::move(inner))
    {}

    // "write <name>", "sync <name>" and "syncdir", for the calls that succeeded
    std::vector<std::string> ops;

    // later syncData calls on `name` return err without syncing
    void failSyncsOf(std::string const& name, int err)
    {
        failName = name;
        failError = err;
    }

    int openDirectory(std::string const& dir) override { return inner->openDirectory(dir); }
    int lockDirectory() override { return inner->lockDirectory(); }
    int createZeroed(std::string const& name, uint64_t bytes) override { return inner->createZeroed(name, bytes); }
    int openForWrite(std::string const& name, int& fd) override
    {
        int err = inner->openForWrite(name, fd);
        if(err == 0)
        {
            names[fd] = name;
        }
        return err;
    }
    int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) override
    {
        int err = inner->writeAt(fd, offset, iov, iovcnt);
        if(err == 0 && (ops.empty() || ops.back() != "write " + names[fd]))
        {
            ops.push_back("write " + names[fd]);
        }
        return err;
    }
    int syncData(int fd) override
    {
        if(failError != 0 && names[fd] == failName)
        {
            return failError;
        }
        int err = inner->syncData(fd);
        if(err == 0)
        {
            ops.push_back("sync " + names[fd]);
        }
        return err;
    }
    int rename(std::string const& from, std::string const& to) override { return inner->rename(from, to); }
    int remove(std::string const& name) override { return inner->remove(name); }
    int syncDirectory() override
    {
        int err = inner->syncDirectory();
        if(err == 0)
        {
            ops.push_back("syncdir");
        }
        return err;
    }
    int readFile(std::string const& name, std::string& out) override { return inner->readFile(name, out); }
    int list(std::vector<std::string>& names) override { return inner->list(names); }
    int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) override
    {
        return inner->freeSpace(free_bytes, total_bytes);
    }
    int closeFile(int fd) override
    {
        names.erase(fd);
        return inner->closeFile(fd);
    }

private:
    std::unique_ptr<chl::WalFileSink> inner;
    std::map<int, std::string> names;
    std::string failName;
    int failError = 0;
};

std::string syncOf(uint64_t segment) { return "sync " + wal::segmentFileName(segment); }

std::string writeOf(uint64_t segment) { return "write " + wal::segmentFileName(segment); }

using Ops = std::vector<std::string>;

bool allZero(std::string const& bytes, std::size_t from)
{
    for(std::size_t i = from; i < bytes.size(); ++i)
    {
        if(bytes[i] != '\0')
        {
            return false;
        }
    }
    return true;
}

class WalRecoveryTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::string pattern = (fs::temp_directory_path() / "wal_recovery_XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        dir = (fs::path(root) / "wal").string();
        fs::create_directories(dir);
    }

    void TearDown() override
    {
        close();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // a fresh sink and manager on the directory, as a restarted keeper makes them
    void open()
    {
        close();
        auto recording = std::make_unique<SyncRecordingSink>(chl::makePosixWalFileSink());
        recorder = recording.get();
        sink = std::make_unique<chl::FaultInjectingWalFileSink>(std::move(recording));
        ASSERT_EQ(sink->openDirectory(dir), 0);
        manager = std::make_unique<chl::WalSegmentManager>(*sink, kSegmentBytes, kEndpoint, dir);
    }

    void close()
    {
        manager.reset();
        sink.reset();
        recorder = nullptr;
    }

    // the writes and syncs that reached the disk since open()
    Ops const& ops() const { return recorder->ops; }

    int run()
    {
        state = chl::RecoveredState{};
        error.clear();
        return chl::recoverWal(*sink, *manager, state, error);
    }

    int recover()
    {
        open();
        return run();
    }

    void write(Segment const& segment, uint64_t size = kSegmentBytes)
    {
        std::string data = segment.bytes;
        if(data.size() < size)
        {
            data.resize(size, '\0');
        }
        writeFile(wal::segmentFileName(segment.number), data);
    }

    void writeFile(std::string const& name, std::string const& bytes)
    {
        std::ofstream out(fs::path(dir) / name, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    std::string readFile(std::string const& name)
    {
        std::ifstream in(fs::path(dir) / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string readSegment(uint64_t segment) { return readFile(wal::segmentFileName(segment)); }

    // every file but the lock, by name
    std::map<std::string, std::string> snapshot()
    {
        std::map<std::string, std::string> files;
        for(auto const& entry: fs::directory_iterator(dir))
        {
            std::string const name = entry.path().filename().string();
            if(name != "lock")
            {
                files[name] = readFile(name);
            }
        }
        return files;
    }

    // what a refused recovery must leave: no files changed, no output, an error
    // that names the directory and the segment
    void expectRefused(std::map<std::string, std::string> const& before, uint64_t segment)
    {
        EXPECT_FALSE(error.empty());
        EXPECT_NE(error.find(dir), std::string::npos) << error;
        EXPECT_NE(error.find(wal::segmentFileName(segment)), std::string::npos) << error;
        EXPECT_TRUE(state.stories.empty());
        EXPECT_TRUE(state.segments.empty());
        EXPECT_EQ(sink->syncCount(), 0u);
        EXPECT_TRUE(ops().empty()) << "a refused recovery wrote or synced before its checks";
        EXPECT_TRUE(snapshot() == before) << "a refused recovery changed a file";
    }

    // Every returned event has its copies and every copy its event, in a listed
    // segment: an event without a copy would let the WAL delete the only
    // segment holding it before the grapher wrote it.
    void expectConsistent()
    {
        for(auto const& [id, story]: state.stories)
        {
            EXPECT_EQ(id, story.wal_story_id);
            EXPECT_FALSE(story.events.empty()) << "story " << id << " returned with no events";
            ASSERT_EQ(story.events.size(), story.copies.size()) << "story " << id;
            for(auto const& [seq, event]: story.events)
            {
                EXPECT_EQ(event.storyId, id);
                EXPECT_TRUE(seqOf(event) == seq);
                auto copies = story.copies.find(seq);
                ASSERT_NE(copies, story.copies.end()) << "event without a copy in story " << id;
                EXPECT_FALSE(copies->second.empty());
                for(auto const& [segment, bytes]: copies->second)
                {
                    EXPECT_NE(std::find(state.segments.begin(), state.segments.end(), segment), state.segments.end())
                            << "copy in unlisted segment " << segment;
                    EXPECT_EQ(bytes, frameOf(event).size());
                }
            }
        }
    }

    Copies copiesOf(chl::StoryId story, chl::LogEvent const& event)
    {
        auto found = state.stories.find(story);
        if(found == state.stories.end())
        {
            return {};
        }
        auto copies = found->second.copies.find(seqOf(event));
        return copies == found->second.copies.end() ? Copies{} : copies->second;
    }

    std::vector<chl::EventSequence> eventsOf(chl::StoryId story)
    {
        std::vector<chl::EventSequence> events;
        auto found = state.stories.find(story);
        if(found != state.stories.end())
        {
            for(auto const& entry: found->second.events) { events.push_back(entry.first); }
        }
        return events;
    }

    thallium::abt argobots;
    std::string root;
    std::string dir;
    std::unique_ptr<chl::FaultInjectingWalFileSink> sink;
    SyncRecordingSink* recorder = nullptr; // owned by sink
    std::unique_ptr<chl::WalSegmentManager> manager;
    chl::RecoveredState state;
    std::string error;
};

} // namespace

TEST_F(WalRecoveryTest, EventsNamesAndCopiesAreRecoveredInLogOrder)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200, 8, 1);
    chl::LogEvent const c = eventOf(kOther, 150);
    chl::LogEvent const d = eventOf(kStory, 300);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    one.event(b);
    one.names(kOther, "chronicle", "other");
    one.event(c);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(d);
    write(one);
    write(two);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(state.segments, (std::vector<uint64_t>{1, 2}));
    EXPECT_FALSE(state.reusable_spare.has_value());
    EXPECT_EQ(state.bytes_read, 2 * kSegmentBytes);
    ASSERT_EQ(state.stories.size(), 2u);
    chl::RecoveredStory const& story = state.stories.at(kStory);
    EXPECT_TRUE(story.has_names);
    EXPECT_EQ(story.chronicle, "chronicle");
    EXPECT_EQ(story.story, "story");
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a), seqOf(b), seqOf(d)}));
    EXPECT_EQ(story.events.at(seqOf(b)).logRecord, b.logRecord);
    EXPECT_EQ(copiesOf(kStory, a), (Copies{{1, frameOf(a).size()}}));
    EXPECT_EQ(copiesOf(kStory, d), (Copies{{2, frameOf(d).size()}}));
    EXPECT_EQ(state.stories.at(kOther).story, "other");
    EXPECT_EQ(eventsOf(kOther), (std::vector<chl::EventSequence>{seqOf(c)}));
    expectConsistent();
    // nothing torn: nothing written, but every segment is synced, oldest first,
    // then the directory
    EXPECT_EQ(ops(), (Ops{syncOf(1), syncOf(2), "syncdir"}));
}

TEST_F(WalRecoveryTest, EmptyDirectoryRecoversNothing)
{
    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_TRUE(state.segments.empty());
    EXPECT_TRUE(state.stories.empty());
    EXPECT_FALSE(state.reusable_spare.has_value());
    EXPECT_EQ(state.bytes_read, 0u);
    EXPECT_EQ(state.oldest_segment_age_secs, 0u);
}

TEST_F(WalRecoveryTest, LockedDirectoryRefusesAndChangesNothing)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    std::string const torn = frameOf(eventOf(kStory, 200));
    one.raw(torn.substr(0, torn.size() / 2));
    write(one);
    auto other = chl::makePosixWalFileSink();
    ASSERT_EQ(other->openDirectory(dir), 0);
    ASSERT_EQ(other->lockDirectory(), 0);
    auto const before = snapshot();

    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find(dir), std::string::npos) << error;
    EXPECT_NE(error.find("locked"), std::string::npos) << error;
    EXPECT_TRUE(state.stories.empty());
    EXPECT_TRUE(snapshot() == before);
}

TEST_F(WalRecoveryTest, TornTailIsZeroedAndSynced)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    chl::LogEvent const c = eventOf(kStory, 300);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(b);
    std::string const torn = frameOf(c);
    uint64_t const torn_at = two.raw(torn.substr(0, torn.size() / 2));
    write(one);
    write(two);
    std::string const older = readSegment(1);
    std::string const newest = readSegment(2);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a), seqOf(b)}));
    expectConsistent();
    // the older segment is synced before the newest one is zeroed
    EXPECT_EQ(ops(), (Ops{syncOf(1), writeOf(2), syncOf(2), "syncdir"}));

    std::string const after = readSegment(2);
    ASSERT_EQ(after.size(), newest.size());
    EXPECT_EQ(after.substr(0, torn_at), newest.substr(0, torn_at));
    EXPECT_TRUE(allZero(after, torn_at));
    EXPECT_EQ(readSegment(1), older);
    EXPECT_FALSE(state.reusable_spare.has_value());

    // a second start finds a clean tail: the same events, nothing written,
    // every segment synced again
    open();
    sink->failWritesWith(EIO);
    ASSERT_EQ(run(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a), seqOf(b)}));
    EXPECT_EQ(ops(), (Ops{syncOf(1), syncOf(2), "syncdir"}));
    EXPECT_EQ(readSegment(2), after);
}

TEST_F(WalRecoveryTest, TornTailBeforeAHeaderOnlySpareIsZeroed)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    std::string const torn = frameOf(eventOf(kStory, 200));
    uint64_t const torn_at = one.raw(torn.substr(0, torn.size() / 2));
    write(one);
    write(Segment(2));
    std::string const spare = readSegment(2);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a)}));
    EXPECT_TRUE(allZero(readSegment(1), torn_at));
    EXPECT_EQ(readSegment(1).substr(0, torn_at), one.bytes.substr(0, torn_at));
    EXPECT_EQ(readSegment(2), spare);
    ASSERT_TRUE(state.reusable_spare.has_value());
    EXPECT_EQ(*state.reusable_spare, 2u);
    EXPECT_EQ(ops(), (Ops{syncOf(2), writeOf(1), syncOf(1), "syncdir"}));
    expectConsistent();
}

TEST_F(WalRecoveryTest, TornWriteWithALaterPageWritten)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    chl::LogEvent const c = eventOf(kStory, 300);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    one.event(b);
    // the page after b never reached the disk; the one after it did
    uint64_t const hole = one.zeros(4096 - one.end() % 4096);
    one.event(c);
    write(one);
    std::string const before = readSegment(1);
    ASSERT_FALSE(allZero(before, hole));

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a), seqOf(b)}));
    std::string const after = readSegment(1);
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(after.substr(0, hole), before.substr(0, hole));
    EXPECT_TRUE(allZero(after, hole));
    EXPECT_EQ(ops(), (Ops{writeOf(1), syncOf(1), "syncdir"}));
    expectConsistent();
}

TEST_F(WalRecoveryTest, HoleRightAfterTheHeaderIsZeroed)
{
    // the first group after a rotation: its first page is missing, a later one
    // landed, so the segment holds no record but is not all zero after its H
    chl::LogEvent const a = eventOf(kStory, 100);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    Segment two(2);
    uint64_t const header_end = two.end();
    two.zeros(4096 - two.end());
    two.names(kStory, "chronicle", "story");
    two.event(eventOf(kStory, 200));
    write(one);
    write(two);
    std::string const older = readSegment(1);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a)}));
    EXPECT_EQ(readSegment(1), older);
    std::string const after = readSegment(2);
    EXPECT_EQ(after.substr(0, header_end), two.bytes.substr(0, header_end));
    EXPECT_TRUE(allZero(after, header_end));
    // it now holds only its header, so the WAL can write into it
    ASSERT_TRUE(state.reusable_spare.has_value());
    EXPECT_EQ(*state.reusable_spare, 2u);
    EXPECT_EQ(ops(), (Ops{syncOf(1), writeOf(2), syncOf(2), "syncdir"}));
    int fd = -1;
    uint64_t data_offset = 0;
    EXPECT_EQ(manager->adoptExisting(2, fd, data_offset), 0) << manager->error();
    EXPECT_EQ(data_offset, header_end);
    if(fd >= 0)
    {
        sink->closeFile(fd);
    }
    expectConsistent();
}

TEST_F(WalRecoveryTest, CleanTailIsNotRewritten)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    one.event(eventOf(kStory, 200));
    write(one);
    auto const before = snapshot();

    open();
    sink->failWritesWith(EIO);
    ASSERT_EQ(run(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(state.stories.at(kStory).events.size(), 2u);
    // no write, and exactly one sync: the segment is still synced
    EXPECT_EQ(sink->syncCount(), 1u);
    EXPECT_EQ(ops(), (Ops{syncOf(1), "syncdir"}));
    EXPECT_TRUE(snapshot() == before);
}

TEST_F(WalRecoveryTest, CleanNewestSegmentIsSyncedBeforeItsEventsAreReturned)
{
    // The keeper wrote its last group and died before the data sync returned.
    // On the same boot the group reads back from the page cache whole, with
    // good CRCs and a clean tail, yet is not on disk. The WAL treats every
    // copy recovery returns as durable and answers a retry of it CL_SUCCESS,
    // so the segment must be synced first.
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(b);
    write(one);
    write(two);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a), seqOf(b)}));
    EXPECT_EQ(ops(), (Ops{syncOf(1), syncOf(2), "syncdir"}));
}

TEST_F(WalRecoveryTest, CleanNewestSegmentWhoseSyncFailsRefusesAndChangesNothing)
{
    for(uint64_t failing: {2u, 1u})
    {
        SCOPED_TRACE("sync of segment " + std::to_string(failing) + " fails");
        for(auto const& entry: fs::directory_iterator(dir)) { fs::remove(entry.path()); }
        Segment one(1);
        one.names(kStory, "chronicle", "story");
        one.event(eventOf(kStory, 100));
        Segment two(2);
        two.names(kStory, "chronicle", "story");
        two.event(eventOf(kStory, 200));
        write(one);
        write(two);
        auto const before = snapshot();

        open();
        recorder->failSyncsOf(wal::segmentFileName(failing), EIO);
        EXPECT_EQ(run(), chl::CL_ERR_UNKNOWN);
        EXPECT_NE(error.find(dir), std::string::npos) << error;
        EXPECT_NE(error.find(wal::segmentFileName(failing)), std::string::npos) << error;
        EXPECT_NE(error.find("sync"), std::string::npos) << error;
        EXPECT_TRUE(state.stories.empty());
        EXPECT_TRUE(state.segments.empty());
        EXPECT_TRUE(snapshot() == before) << "a refused recovery changed a file";
        // nothing after the failed sync, and no directory sync
        EXPECT_EQ(ops(), failing == 2 ? Ops{syncOf(1)} : Ops{});
    }
    // the single-segment case, through the fault sink
    for(auto const& entry: fs::directory_iterator(dir)) { fs::remove(entry.path()); }
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    write(one);
    auto const before = snapshot();
    open();
    sink->failSyncsWith(EIO);
    EXPECT_EQ(run(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find(wal::segmentFileName(1)), std::string::npos) << error;
    EXPECT_TRUE(state.stories.empty());
    EXPECT_TRUE(snapshot() == before);
}

TEST_F(WalRecoveryTest, FailedSyncOfAnOlderSegmentRefusesBeforeTheTornTailIsZeroed)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(eventOf(kStory, 200));
    std::string const torn = frameOf(eventOf(kStory, 300));
    two.raw(torn.substr(0, torn.size() / 2));
    write(one);
    write(two);
    auto const before = snapshot();

    open();
    recorder->failSyncsOf(wal::segmentFileName(1), EIO);
    EXPECT_EQ(run(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find(wal::segmentFileName(1)), std::string::npos) << error;
    EXPECT_TRUE(state.stories.empty());
    EXPECT_TRUE(snapshot() == before) << "the torn tail was zeroed before the refusal";
    EXPECT_TRUE(ops().empty());
}

TEST_F(WalRecoveryTest, DirectoryIsSyncedAndItsFailureRefuses)
{
    // A spare renamed into place just before the crash may lack its directory
    // sync, and the WAL is about to write into it: its name must reach the
    // disk before recovery returns it as reusable.
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    write(one);
    write(Segment(2));
    auto const before = snapshot();

    open();
    sink->failDirectorySyncsWith(EIO);
    EXPECT_EQ(run(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find(dir), std::string::npos) << error;
    EXPECT_NE(error.find("sync"), std::string::npos) << error;
    EXPECT_TRUE(state.stories.empty());
    EXPECT_FALSE(state.reusable_spare.has_value());
    EXPECT_TRUE(snapshot() == before);
    EXPECT_EQ(sink->directorySyncCount(), 1u);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    ASSERT_TRUE(state.reusable_spare.has_value());
    EXPECT_EQ(ops(), (Ops{syncOf(1), syncOf(2), "syncdir"}));
}

TEST_F(WalRecoveryTest, ZeroingFailureRefuses)
{
    for(bool fail_sync: {false, true})
    {
        SCOPED_TRACE(fail_sync ? "sync fails" : "write fails");
        Segment one(1);
        one.names(kStory, "chronicle", "story");
        one.event(eventOf(kStory, 100));
        std::string const torn = frameOf(eventOf(kStory, 200));
        one.raw(torn.substr(0, torn.size() / 2));
        write(one);

        open();
        if(fail_sync)
        {
            sink->failSyncsWith(EIO);
        }
        else
        {
            sink->failWritesWith(EIO);
        }
        EXPECT_EQ(run(), chl::CL_ERR_UNKNOWN);
        EXPECT_NE(error.find(wal::segmentFileName(1)), std::string::npos) << error;
        EXPECT_NE(error.find(dir), std::string::npos) << error;
        EXPECT_TRUE(state.stories.empty());
    }
}

TEST_F(WalRecoveryTest, CrcMismatchBeforeTheTailRefusesAndChangesNothing)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    std::string flipped = frameOf(eventOf(kStory, 200));
    flipped[flipped.size() - 5] ^= 0x01;
    uint64_t const bad_at = one.raw(flipped);
    one.event(eventOf(kStory, 300));
    // the newest segment has a torn tail recovery would otherwise zero
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(eventOf(kStory, 400));
    std::string const torn = frameOf(eventOf(kStory, 500));
    two.raw(torn.substr(0, torn.size() / 2));
    write(one);
    write(two);
    auto const before = snapshot();

    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find("offset " + std::to_string(bad_at)), std::string::npos) << error;
    EXPECT_NE(error.find("CRC"), std::string::npos) << error;
    expectRefused(before, 1);
}

TEST_F(WalRecoveryTest, DataAfterTheEndOfAnOlderSegmentRefuses)
{
    // a zero length followed by records in a segment that is not the newest:
    // its tail was cleaned when it was the newest, so this is damage, and the
    // records after it may be acknowledged events
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    uint64_t const end_at = one.zeros(64);
    one.event(eventOf(kStory, 200));
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(eventOf(kStory, 300));
    write(one);
    write(two);
    auto const before = snapshot();

    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find("offset " + std::to_string(end_at)), std::string::npos) << error;
    expectRefused(before, 1);
}

TEST_F(WalRecoveryTest, UnknownHeaderVersionRefusesAndChangesNothing)
{
    for(uint64_t versioned: {2u, 1u})
    {
        SCOPED_TRACE("version 2 in segment " + std::to_string(versioned));
        for(auto const& entry: fs::directory_iterator(dir)) { fs::remove(entry.path()); }
        Segment one(1, versioned == 1 ? 2 : wal::kFormatVersion);
        one.names(kStory, "chronicle", "story");
        one.event(eventOf(kStory, 100));
        Segment two(2, versioned == 2 ? 2 : wal::kFormatVersion);
        two.names(kStory, "chronicle", "story");
        two.event(eventOf(kStory, 200));
        std::string const torn = frameOf(eventOf(kStory, 300));
        two.raw(torn.substr(0, torn.size() / 2));
        write(one);
        write(two);
        auto const before = snapshot();

        EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
        EXPECT_NE(error.find("version 2"), std::string::npos) << error;
        EXPECT_NE(error.find("version 1"), std::string::npos) << error;
        expectRefused(before, versioned);
    }
}

TEST_F(WalRecoveryTest, UnknownEventCodecVersionRefuses)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    std::string body = frameOf(eventOf(kStory, 200)).substr(wal::kFrameHeaderBytes);
    ASSERT_EQ(body[1], static_cast<char>(chl::kLogEventCodecVersion));
    body[1] = 2;
    uint64_t const bad_at = one.raw(frameOf(body));
    std::string const torn = frameOf(eventOf(kStory, 300));
    one.raw(torn.substr(0, torn.size() / 2));
    write(one);
    auto const before = snapshot();

    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find("codec version 2"), std::string::npos) << error;
    EXPECT_NE(error.find("offset " + std::to_string(bad_at)), std::string::npos) << error;
    expectRefused(before, 1);
}

TEST_F(WalRecoveryTest, UnknownRecordTypeRefuses)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    uint64_t const bad_at = one.raw(frameOf(std::string("X") + le(kStory, 8)));
    one.event(eventOf(kStory, 100));
    std::string const torn = frameOf(eventOf(kStory, 200));
    one.raw(torn.substr(0, torn.size() / 2));
    write(one);
    auto const before = snapshot();

    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find("record type"), std::string::npos) << error;
    EXPECT_NE(error.find("offset " + std::to_string(bad_at)), std::string::npos) << error;
    expectRefused(before, 1);
}

TEST_F(WalRecoveryTest, RecordThatDoesNotDecodeRefuses)
{
    // whole frames with good CRCs whose bodies are cut short
    std::vector<std::string> const bodies = {std::string("N") + le(kStory, 4),
                                             std::string("D") + le(kStory, 4),
                                             frameOf(eventOf(kStory, 100)).substr(wal::kFrameHeaderBytes, 20),
                                             std::string("H")};
    for(std::string const& body: bodies)
    {
        SCOPED_TRACE(std::string("type ") + body[0]);
        Segment one(1);
        one.names(kStory, "chronicle", "story");
        uint64_t const bad_at = one.raw(frameOf(body));
        write(one);
        auto const before = snapshot();

        EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
        EXPECT_NE(error.find("offset " + std::to_string(bad_at)), std::string::npos) << error;
        expectRefused(before, 1);
    }
}

TEST_F(WalRecoveryTest, SegmentWithoutItsHeaderRefuses)
{
    std::string const valid_header = Segment(1).bytes;
    std::string bad_magic = valid_header.substr(wal::kFrameHeaderBytes);
    bad_magic[1] = 'X';
    std::string bad_crc = valid_header;
    bad_crc[bad_crc.size() - 1] ^= 0x01;
    std::vector<std::pair<std::string, std::string>> const cases = {
            {"all zeros", std::string(kSegmentBytes, '\0')},
            {"empty file", std::string()},
            {"names record first", wal::encodeNames(wal::NamesRecord{kStory, "chronicle", "story"})},
            {"bad magic", frameOf(bad_magic)},
            {"header CRC mismatch", bad_crc},
            {"header naming segment 5", Segment(5).bytes},
    };
    for(auto const& [label, bytes]: cases)
    {
        SCOPED_TRACE(label);
        Segment one(1);
        one.names(kStory, "chronicle", "story");
        one.event(eventOf(kStory, 100));
        write(one);
        std::string data = bytes;
        if(!data.empty() && data.size() < kSegmentBytes)
        {
            data.resize(kSegmentBytes, '\0');
        }
        writeFile(wal::segmentFileName(2), data);
        auto const before = snapshot();

        EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
        expectRefused(before, 2);
    }
}

TEST_F(WalRecoveryTest, DroppedKeepsLaterEvents)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    chl::LogEvent const c = eventOf(kStory, 50);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    one.event(b);
    one.dropped(kStory);
    // the story re-created under the same id; no N again in this segment
    one.event(c);
    one.event(a);
    write(one);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(c), seqOf(a)}));
    // the copy of a from before the D went with it
    EXPECT_EQ(copiesOf(kStory, a), (Copies{{1, frameOf(a).size()}}));
    EXPECT_TRUE(state.stories.at(kStory).has_names);
    EXPECT_EQ(state.stories.at(kStory).story, "story");
    expectConsistent();
}

TEST_F(WalRecoveryTest, DroppedKeepsLaterEventsInALaterSegment)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    chl::LogEvent const o = eventOf(kOther, 150);
    chl::LogEvent const c = eventOf(kStory, 300);
    chl::LogEvent const d = eventOf(kStory, 400);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    one.names(kOther, "chronicle", "other");
    one.event(o);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(b);
    two.dropped(kStory);
    Segment three(3);
    three.names(kStory, "chronicle", "story");
    three.event(c);
    three.event(d);
    write(one);
    write(two);
    write(three);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(c), seqOf(d)}));
    EXPECT_EQ(copiesOf(kStory, c), (Copies{{3, frameOf(c).size()}}));
    EXPECT_EQ(eventsOf(kOther), (std::vector<chl::EventSequence>{seqOf(o)}));
    expectConsistent();
}

TEST_F(WalRecoveryTest, DroppedStoryWithNothingAfterIsNotReturned)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    one.names(kOther, "chronicle", "other");
    one.event(eventOf(kOther, 100));
    Segment two(2);
    two.dropped(kStory);
    write(one);
    write(two);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(state.stories.count(kStory), 0u);
    EXPECT_EQ(state.stories.count(kOther), 1u);
    expectConsistent();
}

// A destroy that a failed WAL could not log as a D record, kept in the
// unlogged-drops file: the story's events in segments up to the one named
// are not returned, and events of the story created again in a newer segment
// are. Another story is untouched.
TEST_F(WalRecoveryTest, UnloggedDropRemovesTheStoryUpToItsSegment)
{
    chl::LogEvent const before = eventOf(kStory, 100);
    chl::LogEvent const after = eventOf(kStory, 300);
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(before);
    one.names(kOther, "chronicle", "other");
    one.event(eventOf(kOther, 100));
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(after);
    write(one);
    write(two);
    writeFile(chl::kUnloggedDropsFile, std::to_string(kStory) + " 1\n");

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(after)}));
    EXPECT_EQ(state.stories.count(kOther), 1u);
    // segment 1 is still there, so the entry stays
    EXPECT_EQ(readFile(chl::kUnloggedDropsFile), std::to_string(kStory) + " 1\n");
    expectConsistent();
}

// Once every segment an entry covers is gone, recovery removes the entry.
TEST_F(WalRecoveryTest, UnloggedDropWhoseSegmentsAreGoneIsPruned)
{
    Segment three(3);
    three.names(kStory, "chronicle", "story");
    three.event(eventOf(kStory, 100));
    write(three);
    writeFile(chl::kUnloggedDropsFile, std::to_string(kStory) + " 2\n");

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(eventsOf(kStory).size(), 1u);
    EXPECT_FALSE(fs::exists(fs::path(dir) / chl::kUnloggedDropsFile));
}

TEST_F(WalRecoveryTest, UnloggedDropsFileThatDoesNotParseRefuses)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    write(one);
    writeFile(chl::kUnloggedDropsFile, "not a drop\n");
    EXPECT_EQ(recover(), chl::CL_ERR_UNKNOWN);
    EXPECT_NE(error.find(chl::kUnloggedDropsFile), std::string::npos) << error;
}

TEST_F(WalRecoveryTest, StoryWithoutNamesIsKept)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kOther, 100);
    Segment one(1);
    one.event(a);
    one.names(kOther, "chronicle", "other");
    one.event(b);
    write(one);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    ASSERT_EQ(state.stories.count(kStory), 1u);
    chl::RecoveredStory const& story = state.stories.at(kStory);
    EXPECT_FALSE(story.has_names);
    EXPECT_EQ(story.wal_story_id, kStory);
    EXPECT_EQ(eventsOf(kStory), (std::vector<chl::EventSequence>{seqOf(a)}));
    EXPECT_EQ(copiesOf(kStory, a), (Copies{{1, frameOf(a).size()}}));
    EXPECT_TRUE(state.stories.at(kOther).has_names);
    expectConsistent();
}

TEST_F(WalRecoveryTest, HeaderOnlyNewestSegmentIsReusable)
{
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    write(one);
    write(Segment(2));

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    ASSERT_TRUE(state.reusable_spare.has_value());
    EXPECT_EQ(*state.reusable_spare, 2u);
    EXPECT_EQ(state.segments, (std::vector<uint64_t>{1, 2}));
    // the spare is synced too, and the directory after it
    EXPECT_EQ(ops(), (Ops{syncOf(1), syncOf(2), "syncdir"}));
    // the manager recovery opened can hand it to the WAL
    int fd = -1;
    uint64_t data_offset = 0;
    ASSERT_EQ(manager->adoptExisting(2, fd, data_offset), 0) << manager->error();
    EXPECT_EQ(data_offset, Segment(2).bytes.size());
    sink->closeFile(fd);

    // a newest segment holding records is not reusable
    write(Segment(3), kSegmentBytes);
    Segment four(4);
    four.names(kStory, "chronicle", "story");
    four.event(eventOf(kStory, 200));
    write(four);
    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_FALSE(state.reusable_spare.has_value());
    EXPECT_EQ(state.segments, (std::vector<uint64_t>{1, 2, 3, 4}));
}

TEST_F(WalRecoveryTest, DuplicateCopiesInTwoSegmentsAreBothTracked)
{
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent retried = a;
    retried.logRecord = "the same identity, other bytes";
    Segment one(1);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(retried);
    two.event(a);
    write(one);
    write(two);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    ASSERT_EQ(state.stories.at(kStory).events.size(), 1u);
    // the first copy's bytes are the ones replayed
    EXPECT_EQ(state.stories.at(kStory).events.at(seqOf(a)).logRecord, a.logRecord);
    EXPECT_EQ(copiesOf(kStory, a),
              (Copies{{1, frameOf(a).size()}, {2, frameOf(retried).size()}, {2, frameOf(a).size()}}));
}

TEST_F(WalRecoveryTest, RecoveredCopiesKeepTheirSegmentsFromDeletion)
{
    // what the WAL does with the copies at start: no segment holding a copy is
    // deletable until that event is released
    chl::LogEvent const a = eventOf(kStory, 100);
    chl::LogEvent const b = eventOf(kStory, 200);
    chl::LogEvent const o = eventOf(kOther, 100);
    Segment one(1);
    one.names(kOther, "chronicle", "other");
    one.event(o);
    one.names(kStory, "chronicle", "story");
    one.event(a);
    Segment two(2);
    two.names(kStory, "chronicle", "story");
    two.event(b);
    two.dropped(kStory);
    Segment three(3);
    three.names(kStory, "chronicle", "story");
    three.event(a);
    write(one);
    write(two);
    write(three);
    write(Segment(4));

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    chl::WalReleaseTracker tracker;
    for(auto const& [id, story]: state.stories)
    {
        for(auto const& [seq, copies]: story.copies)
        {
            for(auto const& [segment, bytes]: copies) { tracker.addCopy(id, seq, segment, bytes); }
        }
    }
    EXPECT_EQ(tracker.pendingEvents(), 2u);
    EXPECT_TRUE(tracker.deletable(state.segments, 4).empty());
    tracker.releaseEvent(kOther, seqOf(o));
    EXPECT_EQ(tracker.deletable(state.segments, 4), (std::vector<uint64_t>{1, 2}));
    tracker.releaseEvent(kStory, seqOf(a));
    EXPECT_EQ(tracker.deletable(state.segments, 4), (std::vector<uint64_t>{1, 2, 3}));
}

TEST_F(WalRecoveryTest, BytesReadAndOldestSegmentAge)
{
    uint64_t const now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
    Segment one(1, wal::kFormatVersion, now_ns - 120ull * 1000000000ull);
    one.names(kStory, "chronicle", "story");
    one.event(eventOf(kStory, 100));
    write(one);
    write(Segment(2, wal::kFormatVersion, now_ns), 3 * kSegmentBytes);

    ASSERT_EQ(recover(), chl::CL_SUCCESS) << error;
    EXPECT_EQ(state.bytes_read, 4 * kSegmentBytes);
    EXPECT_GE(state.oldest_segment_age_secs, 119u);
    EXPECT_LE(state.oldest_segment_age_secs, 600u);
}
