#include <gtest/gtest.h>
#include <filesystem>
#include <future>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include "chrono-player/replay/HotReplay.h"

namespace chronolog::player
{
std::unique_ptr<Replay> sequentialArchive(std::shared_ptr<const HotSource> source, HotReplayOptions options);
namespace
{
Event event(int64_t time)
{
    Event e;
    e.id = {1, 2, 3, static_cast<uint64_t>(time)};
    e.hlc = {time, 1};
    e.physical.physical_ns = time;
    e.durability = Durability::Durable;
    return e;
}

class FakeHotSource final: public HotSource
{
public:
    HotFetch response;
    std::function<HotFetch(Hlc)> tail;
    absl::StatusOr<HotFetch> fetchTail(StoryId, Hlc from, const TailStarts&) const override
    {
        return tail ? tail(from) : response;
    }
    absl::StatusOr<HotFetch> fetch(StoryId, const Range&) const override { return response; }
};

class ColdReplay: public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() /
               ("player-cold-" + std::to_string(::getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        auto opened = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        writer = *std::move(opened);
        auto read_only = FileTierStore::OpenReadOnly(root, std::chrono::hours(1));
        ASSERT_TRUE(read_only.ok()) << read_only.status();
        archive = std::shared_ptr<FileTierStore>(*std::move(read_only));
        source = std::make_shared<FakeHotSource>();
        source->response.route_epoch = 7;
        source->response.keepers = {{{"a", 7, {300, 0}, true, false, {200, 0}}, {}},
                                    {{"b", 7, {300, 0}, true, false, {}}, {}}};
        options.archive = archive;
        options.batch_size = 2;
    }
    void TearDown() override
    {
        archive.reset();
        writer.reset();
        std::filesystem::remove_all(root);
    }
    void publish(int64_t time, int64_t start = 100, int64_t end = 200)
    {
        auto record = writer->publish({std::to_string(time), 1, {start, 0}, {end, 0}, {event(time)}, false});
        ASSERT_TRUE(record.ok()) << record.status();
    }
    void read(Hlc start = {100, 0}, StoryId story = 1)
    {
        HotReplay replay(source, options);
        auto stream = replay.read(story, {Range::Axis::Hlc, start, {300, 0}});
        ASSERT_TRUE(stream.ok()) << stream.status();
        events.clear();
        completion.reset();
        for(int i = 0; i < 10; ++i)
        {
            auto batch = (*stream)->next();
            ASSERT_TRUE(batch.ok()) << batch.status();
            if(!*batch)
                return;
            events.insert(events.end(), (**batch).events.begin(), (**batch).events.end());
            if((**batch).completion)
                completion = (**batch).completion;
        }
        FAIL() << "read did not terminate";
    }
    // A Tail that never ends is cancelled after a few seconds so the missing end fails the test instead of hanging it.
    static absl::StatusOr<std::optional<ReplayBatch>> nextWithin(ReplayStream& stream)
    {
        auto pulled = std::async(std::launch::async, [&] { return stream.next(); });
        if(pulled.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            stream.cancel();
        return pulled.get();
    }
    std::filesystem::path root;
    std::unique_ptr<FileTierStore> writer;
    std::shared_ptr<FileTierStore> archive;
    std::shared_ptr<FakeHotSource> source;
    HotReplayOptions options;
    std::vector<Event> events;
    std::optional<Completion> completion;
};

TEST_F(ColdReplay, BatchReadMatchesSequentialArchive)
{
    std::vector<ManifestRecord> records;
    for(int file = 0; file < 24; ++file)
    {
        const int64_t start = 100 + (file / 2) * 10;
        Chunk chunk{std::to_string(file), 1, {start, 0}, {start + 10, 0}, {}};
        for(int i = 0; i < 7; ++i)
        {
            auto e = event(start + (i < 5 ? 1 : i));
            e.id.sequence = file * 7 + i + 1;
            e.physical = {e.hlc.physical_ns, 0, ClockStatus::Synced};
            e.envelope.payload = std::string(64, static_cast<char>('a' + file));
            chunk.events.push_back(e);
        }
        auto published = writer->publish(std::move(chunk));
        ASSERT_TRUE(published.ok()) << published.status();
        records.push_back(*published);
    }
    source->response.archived_below = {300, 0};
    source->tail = [&](Hlc from)
    {
        auto response = source->response;
        response.closed = from >= Hlc{300, 0};
        return response;
    };
    options.batch_size = 13;
    for(int damage = 0; damage < 3; ++damage)
    {
        if(damage == 1)
            std::filesystem::remove(root / records[3].file);
        if(damage == 2)
        {
            writer.reset();
            auto recovered = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
            ASSERT_TRUE(recovered.ok());
            writer = *std::move(recovered);
            std::filesystem::remove(root / records[6].file);
            auto manifest = writer->manifest(1);
            ASSERT_TRUE(manifest.ok());
            ASSERT_TRUE(std::any_of(manifest->begin(),
                                    manifest->end(),
                                    [](const auto& r) { return r.state == ManifestState::Lost; }));
        }
        for(size_t limit: {1u, 3u, 9u, 30u, 1000u})
            for(int path = 0; path < 3; ++path)
            {
                SCOPED_TRACE(::testing::Message() << "damage=" << damage << " limit=" << limit << " path=" << path);
                options.read_max_events = limit;
                auto sequential = sequentialArchive(source, options);
                HotReplay pooled(source, options);
                const Range range{path == 1 ? Range::Axis::Physical : Range::Axis::Hlc, {100, 0}, {300, 0}};
                auto a = path == 2 ? sequential->tail(1, event(100)) : sequential->read(1, range);
                auto b = path == 2 ? pooled.tail(1, event(100)) : pooled.read(1, range);
                ASSERT_TRUE(a.ok()) << a.status();
                ASSERT_TRUE(b.ok()) << b.status();
                bool finished = false;
                for(size_t round = 0; round < 100; ++round)
                {
                    auto x = nextWithin(**a), y = nextWithin(**b);
                    ASSERT_EQ(x.status(), y.status());
                    ASSERT_TRUE(x.ok());
                    ASSERT_EQ(x->has_value(), y->has_value());
                    if(!*x)
                    {
                        finished = true;
                        break;
                    }
                    ASSERT_EQ((**x).events.size(), (**y).events.size());
                    for(size_t i = 0; i < (**x).events.size(); ++i)
                    {
                        const auto& left = (**x).events[i];
                        const auto& right = (**y).events[i];
                        EXPECT_EQ(left.id, right.id);
                        EXPECT_EQ(left.hlc, right.hlc);
                        EXPECT_EQ(left.physical.physical_ns, right.physical.physical_ns);
                        EXPECT_EQ(left.physical.uncertainty_ns, right.physical.uncertainty_ns);
                        EXPECT_EQ(left.physical.status, right.physical.status);
                        EXPECT_EQ(left.envelope.payload, right.envelope.payload);
                        EXPECT_EQ(left.envelope.content_type, right.envelope.content_type);
                        EXPECT_EQ(left.envelope.trace_id, right.envelope.trace_id);
                        EXPECT_EQ(left.envelope.span_id, right.envelope.span_id);
                        EXPECT_EQ(left.envelope.attributes, right.envelope.attributes);
                        EXPECT_EQ(left.durability, right.durability);
                    }
                    ASSERT_EQ((**x).completion.has_value(), (**y).completion.has_value());
                    if((**x).completion)
                    {
                        const auto& left = *(**x).completion;
                        const auto& right = *(**y).completion;
                        EXPECT_EQ(left.complete, right.complete);
                        EXPECT_EQ(left.reason, right.reason);
                        EXPECT_EQ(left.frontier, right.frontier);
                        ASSERT_EQ(left.laggards.size(), right.laggards.size());
                        for(size_t i = 0; i < left.laggards.size(); ++i)
                        {
                            EXPECT_EQ(left.laggards[i].writer_id, right.laggards[i].writer_id);
                            EXPECT_EQ(left.laggards[i].incarnation, right.laggards[i].incarnation);
                            EXPECT_EQ(left.laggards[i].frontier, right.laggards[i].frontier);
                        }
                    }
                }
                EXPECT_TRUE(finished);
            }
    }
}

TEST_F(ColdReplay, UnacknowledgedEventsBelowTheBoundaryAreKept)
{
    publish(140);
    source->response.keepers[0].events = {event(120)};
    read();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].hlc, (Hlc{120, 1}));
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, EventHeldByTwoKeepersIsReturnedOnce)
{
    publish(140);
    for(auto& k: source->response.keepers) k.events = {event(140)};
    read();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].id, event(140).id);
}

TEST_F(ColdReplay, AKeeperThatDidNotAnswerWidensTheArchiveReadAndMarksTheReplyIncomplete)
{
    publish(140);
    publish(250, 200, 300);
    source->response.keepers[1].frontier.answered = false;
    read();
    ASSERT_EQ(events.size(), 2);
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, ATruncatedAnswerMarksTheReplyIncompleteWithoutWideningTheRead)
{
    publish(140);
    publish(250, 200, 300);
    source->response.keepers[1].frontier.truncated = true;
    read();
    EXPECT_TRUE(events.empty());
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion->frontier, (Hlc{100, 0}));
}

TEST_F(ColdReplay, BoundaryIsTheHighestWatermarkAKeeperReports)
{
    publish(140);
    publish(250, 200, 300);
    source->response.keepers[0].frontier.evicted_below = {150, 0};
    source->response.keepers[1].frontier.evicted_below = {260, 0};
    read();
    ASSERT_EQ(events.size(), 2);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, ChunkWrittenBeforeThePlayerSeesTheFileIsStillReplayed)
{
    publish(140);
    source->response.keepers[0].frontier.evicted_below = {};
    source->response.keepers[0].events = {event(140)};
    read();
    ASSERT_EQ(events.size(), 1);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, KeeperFreesTheChunkOnceThePlayerCanReadTheFile)
{
    publish(140);
    auto stale = archive->manifest(1);
    EXPECT_TRUE(absl::IsNotFound(stale.status()));
    read();
    ASSERT_EQ(events.size(), 1);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, AStoryTheArchiveNeverHeardOfReadsCompleteWhenNothingIsEvicted)
{
    for(auto& keeper: source->response.keepers) keeper.frontier.evicted_below = {};
    source->response.keepers[0].events = {event(140)};
    read({100, 0}, 2);
    ASSERT_EQ(events.size(), 1);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, NoKeepersReadsTheArchiveAloneAndIsSourceFailed)
{
    publish(140);
    source->response.keepers.clear();
    read();
    ASSERT_EQ(events.size(), 1);
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, ADeletionAppendedAfterTheViewOpenedRemovesTheFileButKeepsTheWatermark)
{
    publish(140);
    ASSERT_TRUE(archive->refreshNow().ok());
    ASSERT_EQ(archive->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}})->size(), 1);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_TRUE(writer->eraseFile(records->front().file).ok());
    ASSERT_TRUE(archive->refreshNow().ok());
    auto archived = archive->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    ASSERT_TRUE(archived.ok());
    EXPECT_TRUE(archived->empty());
    auto watermark = archive->contiguousWatermark(1);
    ASSERT_TRUE(watermark.ok());
    EXPECT_EQ(*watermark, (Hlc{200, 0}));
}

TEST_F(ColdReplay, APartiallyWrittenManifestRecordIsNotVisibleUntilItIsComplete)
{
    publish(140);
    auto other = FileTierStore::Open(root, "other", {{1, {100, 0}}});
    ASSERT_TRUE(other.ok());
    ASSERT_TRUE((*other)->publish({"second", 1, {100, 0}, {200, 0}, {event(150)}, false}).ok());
    const auto log = root / "manifest" / "other.log";
    std::ostringstream whole;
    whole << std::ifstream(log).rdbuf();
    const std::string complete = whole.str();
    ASSERT_GT(complete.size(), 10u);
    other->reset();
    std::ofstream(log, std::ios::trunc) << complete.substr(0, complete.size() - 10);
    ASSERT_TRUE(archive->refreshNow().ok());
    EXPECT_EQ(archive->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}})->size(), 1) << "a torn record was consumed";
    std::ofstream(log, std::ios::trunc) << complete;
    ASSERT_TRUE(archive->refreshNow().ok());
    EXPECT_EQ(archive->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}})->size(), 2)
            << "the completed record was never picked up";
}

TEST_F(ColdReplay, MissingArchiveFileIsSourceFailed)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    std::filesystem::remove(root / records->front().file);
    read();
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, AFileRemovedByAStoryDestroyEndsSourceFailed)
{
    publish(140);
    source->response.keepers[0].events = {event(120)};
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(writer->eraseFile(records->front().file).ok());
    read();
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].hlc, (Hlc{120, 1}));
}

TEST_F(ColdReplay, ARetentionDeletionWithoutATombstoneStaysComplete)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_TRUE(writer->eraseFile(records->front().file).ok());
    read();
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, APhysicalReadAfterAStoryDestroyEndsSourceFailed)
{
    auto e = event(140);
    e.physical = {150, 5, ClockStatus::Synced};
    Chunk chunk{"physical-destroyed", 1, {100, 0}, {200, 0}, {e}, false};
    chunk.physical_policy = true;
    ASSERT_TRUE(writer->publish(chunk).ok());
    source->response.physical_policy = true;
    for(auto& k: source->response.keepers) k.frontier.physical_frontier = 400;
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(writer->eraseFile(records->front().file).ok());
    HotReplay replay(source, options);
    auto stream = replay.read(1, {Range::Axis::Physical, {145, 0}, {155, 0}});
    ASSERT_TRUE(stream.ok());
    std::optional<Completion> physical;
    for(int i = 0; i < 4 && !physical; ++i)
    {
        auto batch = (*stream)->next();
        ASSERT_TRUE(batch.ok());
        ASSERT_TRUE(*batch);
        physical = (**batch).completion;
    }
    ASSERT_TRUE(physical);
    EXPECT_FALSE(physical->complete);
    EXPECT_EQ(physical->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, ATailOverAFileRemovedByAStoryDestroyEndsSourceFailed)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(writer->eraseFile(records->front().file).ok());
    ASSERT_TRUE(archive->refreshNow().ok());
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, DisabledArchiveWithEvictedEventsIsSourceFailed)
{
    options.archive.reset();
    read();
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, ZeroEvictionFloorDoesNotNeedAnArchive)
{
    options.archive.reset();
    source->response.keepers[0].frontier.evicted_below = {};
    source->response.keepers[0].events = {event(140)};
    read();
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
    ASSERT_EQ(events.size(), 1);
}

TEST_F(ColdReplay, PhysicalArchiveCountsOnlyIntervalMatches)
{
    std::vector<Event> archived;
    for(int64_t i = 100; i < 140; ++i)
    {
        auto e = event(i);
        e.physical = {1000, 0, ClockStatus::Synced};
        archived.push_back(e);
    }
    auto e = event(140);
    e.physical = {150, 5, ClockStatus::Synced};
    archived.push_back(e);
    Chunk chunk{"physical-bounded", 1, {100, 0}, {200, 0}, archived, false};
    chunk.physical_policy = true;
    ASSERT_TRUE(writer->publish(chunk).ok());
    source->response.physical_policy = true;
    for(auto& k: source->response.keepers) k.frontier.physical_frontier = 400;
    options.read_max_events = 1;
    HotReplay replay(source, options);
    auto stream = replay.read(1, {Range::Axis::Physical, {151, 0}, {155, 0}});
    ASSERT_TRUE(stream.ok());
    auto batch = (*stream)->next();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1);
    EXPECT_EQ((**batch).events[0].id, e.id);
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_TRUE((**final).completion->complete);
}
TEST_F(ColdReplay, LegacyArchiveMarkerDisablesPruningAndCompletion)
{
    auto e = event(140);
    e.hlc = {1000000000000000LL, 0};
    e.physical = {150, 0, ClockStatus::Synced};
    Chunk chunk{"physical-legacy", 1, e.hlc, {e.hlc.physical_ns + 1, 0}, {e}, false};
    ASSERT_TRUE(writer->publish(chunk).ok());
    source->response.physical_policy = true;
    for(auto& k: source->response.keepers) k.frontier.physical_frontier = 400;
    HotReplay replay(source, options);
    auto stream = replay.read(1, {Range::Axis::Physical, {150, 0}, {151, 0}});
    ASSERT_TRUE(stream.ok());
    auto batch = (*stream)->next();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1);
    EXPECT_EQ((**batch).events[0].id, e.id);
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::PhysicalAxisUnbounded);
}

TEST_F(ColdReplay, PhysicalReadDoesNotTreatAnHlcEvictionFloorAsPhysicalTime)
{
    auto e = event(140);
    e.physical.physical_ns = 250;
    auto record = writer->publish({"physical", 1, {100, 0}, {200, 0}, {e}, false});
    ASSERT_TRUE(record.ok());
    HotReplay replay(source, options);
    auto stream = replay.read(1, {Range::Axis::Physical, {100, 0}, {300, 0}});
    ASSERT_TRUE(stream.ok());
    auto batch = (*stream)->next();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1);
    EXPECT_EQ((**batch).events.front().id, e.id);
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::PhysicalAxisUnbounded);
}

TEST_F(ColdReplay, TailCatchesUpFromArchiveExclusivelyAfterPosition)
{
    publish(120);
    publish(140);
    source->response.keepers[0].events = {event(140), event(250)};
    source->response.closed = true;
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(120));
    ASSERT_TRUE(stream.ok());
    auto batch = (*stream)->next();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 2);
    EXPECT_EQ((**batch).events[0].hlc, (Hlc{140, 1}));
    EXPECT_EQ((**batch).events[1].hlc, (Hlc{250, 1}));
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_FALSE((**final).completion->complete);
}

// The Catalog decides: a recorded tombstone is evidence that ends the Tail FAILED_PRECONDITION once confirmed, and
// ends it SOURCE_FAILED while the Catalog does not confirm (PI 14:50, I6.7, I13.11).
TEST_F(ColdReplay, TailCancelDoesNotWaitForAnArchiveRead)
{
    for(auto& k: source->response.keepers)
    {
        k.frontier.evicted_below = {};
        k.frontier.sealed = {130, 0};
    }
    source->response.keepers[0].events = {event(120)};
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto first = (*stream)->next();
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(*first);
    ASSERT_EQ((**first).events.size(), 1u);
    auto proto = FileTierStore::Open(root, "proto", {{1, {100, 0}}}, std::make_shared<ProtoChunkCodec>());
    ASSERT_TRUE(proto.ok());
    auto record = (*proto)->publish({"blocked", 1, {130, 0}, {200, 0}, {event(140)}, false});
    ASSERT_TRUE(record.ok());
    const auto file = root / record->file;
    ASSERT_TRUE(std::filesystem::remove(file));
    ASSERT_EQ(::mkfifo(file.c_str(), 0600), 0);
    const int pipe = ::open(file.c_str(), O_RDWR | O_NONBLOCK);
    ASSERT_GE(pipe, 0);
    const int watch = ::inotify_init1(IN_NONBLOCK);
    ASSERT_GE(watch, 0);
    ASSERT_GE(::inotify_add_watch(watch, file.c_str(), IN_OPEN), 0);
    for(auto& k: source->response.keepers)
    {
        k.frontier.evicted_below = {200, 0};
        k.frontier.sealed = {300, 0};
    }
    auto next = std::async(std::launch::async, [&] { return (*stream)->next(); });
    pollfd opened{watch, POLLIN, 0};
    const bool reading = ::poll(&opened, 1, 5000) == 1;
    auto cancel = std::async(std::launch::async, [&] { (*stream)->cancel(); });
    const bool cancelled = cancel.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    // Closing the only writer releases the archive reader even when the cancel assertion fails.
    ::close(pipe);
    ::close(watch);
    cancel.get();
    (void)next.get();
    EXPECT_TRUE(reading);
    EXPECT_TRUE(cancelled);
}

TEST_F(ColdReplay, TailConfirmsATombstoneFirstSeenDuringArchiveRefresh)
{
    publish(140);
    for(auto& k: source->response.keepers) k.frontier.sealed = {150, 0};
    unsigned confirmations = 0;
    options.story_live = [&](StoryId)
    {
        ++confirmations;
        return absl::FailedPreconditionError("destroyed");
    };
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto first = (*stream)->next();
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(*first);
    ASSERT_EQ((**first).events.size(), 1u);
    ASSERT_TRUE(writer->tombstone(1).ok());
    for(auto& k: source->response.keepers) k.frontier.sealed = {300, 0};
    auto next = nextWithin(**stream);
    EXPECT_EQ(next.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(confirmations, 1u);
}

TEST_F(ColdReplay, TailDoesNotReconfirmAnUnchangedRefusalSet)
{
    options.archive.reset();
    options.tail_poll = std::chrono::milliseconds(1);
    unsigned calls = 0, confirmations = 0;
    source->tail = [&](Hlc)
    {
        auto reply = source->response;
        for(auto& k: reply.keepers) k.frontier.evicted_below = {};
        auto& refused = reply.keepers[0].frontier;
        refused.answered = false;
        refused.status = absl::StatusCode::kFailedPrecondition;
        refused.instance = ++calls < 6 ? "one" : "two";
        reply.closed = calls == 10;
        return reply;
    };
    options.story_live = [&](StoryId)
    {
        ++confirmations;
        return absl::OkStatus();
    };
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto next = nextWithin(**stream);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(calls, 10u);
    EXPECT_EQ(confirmations, 2u);
}

TEST_F(ColdReplay, TailWithNoKeepersDoesNotAdvanceToAnAbandonedRange)
{
    publish(140);
    source->response.keepers.clear();
    source->response.abandoned = {{Range::Axis::Hlc, {200, 0}, {300, 0}}};
    source->response.closed = true;
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto next = (*stream)->next();
    ASSERT_TRUE(next.ok());
    ASSERT_TRUE(*next);
    ASSERT_TRUE((**next).completion);
    EXPECT_EQ((**next).completion->frontier, event(110).hlc);
}

TEST_F(ColdReplay, TailReadsAnArchiveLargerThanItsCapAcrossRounds)
{
    ASSERT_TRUE(
            writer->publish({"large", 1, {100, 0}, {150, 0}, {event(110), event(120), event(130), event(140)}, false})
                    .ok());
    ASSERT_TRUE(writer->publish({"later", 1, {150, 0}, {200, 0}, {event(150), event(160), event(170)}, false}).ok());
    options.read_max_events = 2;
    options.batch_size = 100;
    unsigned calls = 0;
    source->tail = [&](Hlc from)
    {
        ++calls;
        auto reply = source->response;
        reply.closed = from >= Hlc{200, 0};
        return reply;
    };
    HotReplay replay(source, options);
    Event start;
    start.id.story_id = 1;
    auto stream = replay.tail(1, start);
    ASSERT_TRUE(stream.ok());
    std::vector<Event> got;
    for(unsigned i = 0; i < 10; ++i)
    {
        auto next = nextWithin(**stream);
        ASSERT_TRUE(next.ok());
        if(!*next || (**next).completion)
            break;
        EXPECT_LE((**next).events.size(), 2u);
        got.insert(got.end(), (**next).events.begin(), (**next).events.end());
    }
    ASSERT_EQ(got.size(), 7u);
    for(size_t i = 0; i < got.size(); ++i) EXPECT_EQ(got[i].hlc, event(110 + 10 * i).hlc);
    EXPECT_GE(calls, 4u);
}

TEST_F(ColdReplay, TailDeliversAnEventLargerThanItsByteShare)
{
    auto first = event(110);
    first.envelope.payload = std::string(100, 'a');
    auto tied = first;
    tied.id.writer_id = 3;
    tied.envelope.payload = std::string(100, 'b');
    auto third = tied;
    third.id.writer_id = 4;
    auto fourth = tied;
    fourth.id.writer_id = 5;
    auto later = event(130);
    later.envelope.payload = std::string(100, 'c');
    ASSERT_TRUE(writer->publish({"oversized", 1, {100, 0}, {200, 0}, {first, tied, third, fourth, later}, false}).ok());
    options.tail_max_bytes = 144;
    options.read_max_events = 2;
    options.batch_size = 10;
    HotReplay replay(source, options);
    Event start;
    start.id.story_id = 1;
    auto stream = replay.tail(1, start);
    ASSERT_TRUE(stream.ok());
    auto batch = nextWithin(**stream);
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 4u);
    EXPECT_EQ((**batch).events[2].id, third.id);
    EXPECT_EQ((**batch).events[3].id, fourth.id);
    EXPECT_EQ((**batch).events[0].id, first.id);
    EXPECT_EQ((**batch).events[1].id, tied.id);
    EXPECT_EQ((**batch).events[0].envelope.payload, first.envelope.payload);
    EXPECT_EQ((**batch).events[1].envelope.payload, tied.envelope.payload);
    EXPECT_FALSE((**batch).completion);
    batch = nextWithin(**stream);
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(*batch);
    ASSERT_EQ((**batch).events.size(), 1u);
    EXPECT_EQ((**batch).events[0].id, later.id);
    EXPECT_EQ((**batch).events[0].envelope.payload, later.envelope.payload);
    EXPECT_FALSE((**batch).completion);
    (*stream)->cancel();
}

TEST_F(ColdReplay, ATailOverATombstonedStoryEndsFailedPreconditionWhenTheCatalogConfirms)
{
    publish(140);
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(archive->refreshNow().ok());
    options.story_live = [](StoryId) { return absl::FailedPreconditionError("story is tombstoned"); };
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto next = nextWithin(**stream);
    ASSERT_FALSE(next.ok());
    EXPECT_EQ(next.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST_F(ColdReplay, ATailOverATombstonedStoryEndsSourceFailedWhileTheCatalogDoesNotConfirm)
{
    publish(140);
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(archive->refreshNow().ok());
    options.story_live = [](StoryId) { return absl::OkStatus(); };
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::SourceFailed);
}

TEST_F(ColdReplay, ATailWhoseKeeperIsDownStillEndsWhenTheCatalogConfirmsARecordedTombstone)
{
    publish(140);
    ASSERT_TRUE(writer->tombstone(1).ok());
    ASSERT_TRUE(archive->refreshNow().ok());
    source->response.keepers[1].frontier.answered = false;
    options.story_live = [](StoryId) { return absl::FailedPreconditionError("story is tombstoned"); };
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto next = nextWithin(**stream);
    ASSERT_FALSE(next.ok());
    EXPECT_EQ(next.status().code(), absl::StatusCode::kFailedPrecondition);
}

// Archived events wait for the lowest seal exactly as hot events do: an event a second Keeper has not caught up to
// could still arrive below them.
TEST_F(ColdReplay, TailWithholdsAnArchivedEventAtOrAboveTheLowestSeal)
{
    publish(140, 100, 400);
    publish(450, 400, 500);
    source->response.keepers[0].frontier.evicted_below = {500, 0};
    source->response.keepers[0].frontier.sealed = {600, 0};
    source->response.keepers[1].frontier.sealed = {300, 0};
    source->response.closed = true;
    auto run = [&](std::vector<Event>& events, std::optional<Completion>& completion)
    {
        HotReplay replay(source, options);
        auto stream = replay.tail(1, event(120));
        ASSERT_TRUE(stream.ok());
        for(int i = 0; i < 10; ++i)
        {
            auto batch = (*stream)->next();
            ASSERT_TRUE(batch.ok());
            if(!*batch)
                return;
            events.insert(events.end(), (**batch).events.begin(), (**batch).events.end());
            if((**batch).completion)
                completion = (**batch).completion;
        }
    };
    std::vector<Event> events;
    std::optional<Completion> completion;
    run(events, completion);
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].hlc, (Hlc{140, 1}));
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->frontier, (Hlc{300, 0}));
    source->response.keepers[1].frontier.sealed = {600, 0};
    events.clear();
    run(events, completion);
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[1].hlc, (Hlc{450, 1}));
}

TEST_F(ColdReplay, LimitCutsAtRecordStartAndContinuationHasNoGapOrDuplicate)
{
    options.read_max_events = 3;
    options.batch_size = 1;
    source->response.keepers[0].frontier.evicted_below = {300, 0};
    source->response.keepers[0].events = {event(145), event(250)};
    auto first = writer->publish({"first", 1, {100, 0}, {150, 0}, {event(120), event(140)}, false});
    ASSERT_TRUE(first.ok());
    auto second = writer->publish({"second", 1, {150, 0}, {200, 0}, {event(160), event(180)}, false});
    ASSERT_TRUE(second.ok());
    publish(220, 200, 250);
    read();
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion->frontier, (Hlc{150, 0}));
    ASSERT_EQ(events.size(), 3);
    EXPECT_EQ(events.back().id, event(145).id);
    std::vector<Event> all = events;
    Hlc cut = completion->frontier;
    read(cut);
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion->frontier, (Hlc{200, 0}));
    all.insert(all.end(), events.begin(), events.end());
    read(completion->frontier);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
    all.insert(all.end(), events.begin(), events.end());
    ASSERT_EQ(all.size(), 7);
    const std::vector<int64_t> expected{120, 140, 145, 160, 180, 220, 250};
    for(size_t j = 0; j < all.size(); ++j) EXPECT_EQ(all[j].hlc.physical_ns, expected[j]);
}

TEST_F(ColdReplay, AFileLargerThanTheLimitIsReturnedWhole)
{
    options.read_max_events = 1;
    options.batch_size = 1;
    auto record = writer->publish({"large", 1, {100, 0}, {200, 0}, {event(120), event(140)}, false});
    ASSERT_TRUE(record.ok());
    read();
    ASSERT_EQ(events.size(), 2);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
}

TEST_F(ColdReplay, TailEndsSourceFailedOnALostWindow)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    std::filesystem::remove(root / records->front().file);
    writer.reset();
    auto recovered = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
    ASSERT_TRUE(recovered.ok());
    writer = *std::move(recovered);
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    EXPECT_TRUE((**final).events.empty());
    ASSERT_TRUE((**final).completion);
    EXPECT_FALSE((**final).completion->complete);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::SourceFailed);
    EXPECT_EQ((**final).completion->frontier, event(110).hlc);
    auto eof = (*stream)->next();
    ASSERT_TRUE(eof.ok());
    EXPECT_FALSE(*eof);
}

TEST_F(ColdReplay, TailEndsSourceFailedOnAnArchiveReadError)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    std::filesystem::remove(root / records->front().file);
    HotReplay replay(source, options);
    auto stream = replay.tail(1, event(110));
    ASSERT_TRUE(stream.ok());
    auto final = (*stream)->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_EQ((**final).completion->reason, IncompleteReason::SourceFailed);
    auto eof = (*stream)->next();
    ASSERT_TRUE(eof.ok());
    EXPECT_FALSE(*eof);
}

TEST_F(ColdReplay, ReadOnlyViewMergesEveryWriterAndNeverScansDataFiles)
{
    publish(140);
    auto other = FileTierStore::Open(root, "other", {{1, {100, 0}}});
    ASSERT_TRUE(other.ok());
    auto record = (*other)->publish({"second", 1, {100, 0}, {200, 0}, {event(150)}, false});
    ASSERT_TRUE(record.ok());
    ASSERT_TRUE((*other)->compact().ok());
    read();
    ASSERT_EQ(events.size(), 2);
    other->reset();
    std::filesystem::remove(root / "manifest" / "other.log");
    std::filesystem::remove(root / "manifest" / "other.snap");
    ASSERT_TRUE(archive->refreshNow().ok());
    auto archived = archive->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    ASSERT_TRUE(archived.ok());
    ASSERT_EQ(archived->size(), 1);
    EXPECT_EQ(archived->front().id, event(140).id);
}

TEST_F(ColdReplay, ReadOnlyViewDoesNotCreateOrMutateManifestFiles)
{
    publish(140);
    auto status = writer->compact();
    ASSERT_TRUE(status.ok());
    std::map<std::filesystem::path, uintmax_t> before;
    for(const auto& f: std::filesystem::directory_iterator(root / "manifest")) before[f.path()] = f.file_size();
    read();
    EXPECT_FALSE(archive->publish({"bad", 1, {100, 0}, {200, 0}, {}, false}).ok());
    EXPECT_FALSE(archive->compact().ok());
    auto watermark = archive->contiguousWatermark(1);
    ASSERT_TRUE(watermark.ok());
    EXPECT_EQ(*watermark, (Hlc{200, 0}));
    std::map<std::filesystem::path, uintmax_t> after;
    for(const auto& f: std::filesystem::directory_iterator(root / "manifest")) after[f.path()] = f.file_size();
    EXPECT_EQ(before, after);
}
} // namespace
} // namespace chronolog::player
