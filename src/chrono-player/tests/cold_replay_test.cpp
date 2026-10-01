#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>
#include "chrono-player/replay/HotReplay.h"

namespace chronolog::player
{
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
    void read()
    {
        HotReplay replay(source, options);
        auto stream = replay.read(1, {Range::Axis::Hlc, {100, 0}, {300, 0}});
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
    std::filesystem::path root;
    std::unique_ptr<FileTierStore> writer;
    std::shared_ptr<FileTierStore> archive;
    std::shared_ptr<FakeHotSource> source;
    HotReplayOptions options;
    std::vector<Event> events;
    std::optional<Completion> completion;
};

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
    ASSERT_EQ(events.size(), 1);
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->reason, IncompleteReason::Truncated);
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

TEST_F(ColdReplay, LostWindowBelowWatermarkIsSourceFailed)
{
    publish(140);
    auto records = writer->manifest(1);
    ASSERT_TRUE(records.ok());
    std::filesystem::remove(root / records->front().file);
    writer.reset();
    auto recovered = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
    ASSERT_TRUE(recovered.ok()) << recovered.status();
    writer = *std::move(recovered);
    auto watermark = writer->contiguousWatermark(1);
    ASSERT_TRUE(watermark.ok());
    EXPECT_EQ(*watermark, (Hlc{200, 0}));
    read();
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
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
