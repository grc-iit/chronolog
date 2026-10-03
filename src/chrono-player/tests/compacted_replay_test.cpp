#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <unistd.h>
#include "tier/ChunkCodec.h"
#include "chrono-player/replay/HotReplay.h"

namespace chronolog::player
{
namespace
{
namespace fs = std::filesystem;

Event event(int64_t time, uint64_t seq)
{
    Event e;
    e.id = {1, 2, 3, static_cast<uint64_t>(time) * 10 + seq + 1};
    e.hlc = {time, 1};
    e.physical = {time, 0, ClockStatus::Synced};
    e.durability = Durability::Durable;
    e.envelope.payload = "payload-" + std::to_string(e.id.sequence);
    return e;
}

// Window [start, start + 50) holds a tie group of three events at start + 5 and one event at start + 20.
std::vector<Event> windowEvents(int64_t start)
{
    return {event(start + 5, 0), event(start + 5, 1), event(start + 5, 2), event(start + 20, 0)};
}

// A Keeper that answers a bounded fetch with at most target + 1 events and marks the answer truncated, as
// KeeperHotSource asks it to; a Tail round closes once it starts at `closed_at`.
class KeeperModel final: public HotSource
{
public:
    HotFetch response;
    Hlc closed_at = maxHlc();
    absl::StatusOr<HotFetch> fetch(StoryId, const Range&) const override { return response; }
    absl::StatusOr<HotFetch> fetchRead(StoryId, const Range& range, size_t target) const override
    {
        auto reply = response;
        for(auto& keeper: reply.keepers)
        {
            std::erase_if(keeper.events, [&](const Event& e) { return e.hlc < range.start || e.hlc >= range.end; });
            std::stable_sort(keeper.events.begin(), keeper.events.end(), ReplayLess);
            if(target != SIZE_MAX && keeper.events.size() > target + 1)
            {
                keeper.events.resize(target + 1);
                keeper.frontier.truncated = true;
            }
        }
        return reply;
    }
    absl::StatusOr<HotFetch> fetchTail(StoryId, Hlc from, const TailStarts&) const override
    {
        auto reply = response;
        reply.closed = from >= closed_at;
        return reply;
    }
};

struct Result
{
    std::vector<Event> events;
    std::optional<Completion> completion;
};

void expectSame(const Result& expected, const Result& actual)
{
    ASSERT_EQ(actual.events.size(), expected.events.size());
    for(size_t i = 0; i < expected.events.size(); ++i)
    {
        EXPECT_EQ(actual.events[i].id, expected.events[i].id) << i;
        EXPECT_EQ(actual.events[i].hlc, expected.events[i].hlc) << i;
        EXPECT_EQ(actual.events[i].durability, expected.events[i].durability) << i;
        EXPECT_EQ(actual.events[i].envelope.payload, expected.events[i].envelope.payload) << i;
    }
    ASSERT_TRUE(expected.completion);
    ASSERT_TRUE(actual.completion);
    EXPECT_EQ(actual.completion->complete, expected.completion->complete);
    EXPECT_EQ(actual.completion->reason, expected.completion->reason);
    EXPECT_EQ(actual.completion->frontier, expected.completion->frontier);
}

// The Player's read-only archive view loads every file through `load`; at the load numbered `trigger` the writer
// commits one compaction switch and unlinks its inputs, after the reader planned against them.
class CompactedReplay: public ::testing::Test
{
protected:
    void open(const std::string& name, size_t threads)
    {
        archive.reset();
        writer.reset();
        if(!root.empty())
            fs::remove_all(root);
        root = fs::temp_directory_path() /
               ("player-compacted-" + std::to_string(::getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + "-" + name);
        fs::remove_all(root);
        auto opened = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        writer = *std::move(opened);
        auto read_only = FileTierStore::OpenReadOnly(
                root,
                std::chrono::hours(1),
                [this](const fs::path& file) { return load(file); },
                threads);
        ASSERT_TRUE(read_only.ok()) << read_only.status();
        archive = std::shared_ptr<FileTierStore>(*std::move(read_only));
        options = {};
        options.archive = archive;
        options.batch_size = 7;
        options.tail_poll = std::chrono::milliseconds(1);
        loads = 0;
        trigger = 0;
        vanished = 0;
        compacted = 0;
        loaded.clear();
        inputs.clear();
        source = std::make_shared<KeeperModel>();
        source->response.route_epoch = 7;
        source->response.keepers = {{{"a", 7, {2000, 0}}, {}}, {{"b", 7, {2000, 0}}, {}}};
    }
    void TearDown() override
    {
        archive.reset();
        writer.reset();
        if(!root.empty())
            fs::remove_all(root);
    }
    absl::StatusOr<ChunkBytes> load(const fs::path& file)
    {
        {
            std::lock_guard lock(mu);
            loaded.push_back(file);
        }
        if(++loads == trigger)
        {
            auto result = writer->compactOnce(eager());
            compacted = result.ok() ? result->inputs : 0;
        }
        auto bytes = LoadChunkFile(file);
        if(!bytes.ok() && ArchiveFileVanished(bytes.status()))
            ++vanished;
        return bytes;
    }
    static CompactionPolicy eager()
    {
        CompactionPolicy policy;
        policy.min_files = 2;
        policy.min_age = std::chrono::seconds(0);
        policy.io_bytes_per_sec = policy.io_burst_bytes = uint64_t{1} << 30;
        return policy;
    }
    // Publishes `count` contiguous windows from `first`; returns their events.
    std::vector<Event> publish(int64_t first, int count, bool physical_policy = false)
    {
        std::vector<Event> all;
        for(int i = 0; i < count; ++i)
        {
            const int64_t start = first + 50 * i;
            auto events = windowEvents(start);
            Chunk chunk{"w" + std::to_string(start), 1, {start, 0}, {start + 50, 0}, events, false, physical_policy};
            auto record = writer->publish(chunk);
            EXPECT_TRUE(record.ok()) << record.status();
            if(record.ok())
                inputs.push_back(root / record->file);
            all.insert(all.end(), events.begin(), events.end());
        }
        return all;
    }
    static Result drain(absl::StatusOr<std::unique_ptr<ReplayStream>> stream)
    {
        Result result;
        EXPECT_TRUE(stream.ok()) << stream.status();
        if(!stream.ok())
            return result;
        for(int i = 0; i < 1000; ++i)
        {
            auto batch = (*stream)->next();
            EXPECT_TRUE(batch.ok()) << batch.status();
            if(!batch.ok() || !*batch)
                return result;
            result.events.insert(result.events.end(), (**batch).events.begin(), (**batch).events.end());
            if((**batch).completion)
                result.completion = (**batch).completion;
        }
        ADD_FAILURE() << "read did not terminate";
        return result;
    }
    // A Tail that does not end within five seconds is cancelled, so the missing end fails the test.
    Result tail()
    {
        HotReplay replay(source, options);
        Event position;
        position.id.story_id = 1;
        auto stream = replay.tail(1, position);
        Result result;
        EXPECT_TRUE(stream.ok()) << stream.status();
        if(!stream.ok())
            return result;
        for(int i = 0; i < 1000; ++i)
        {
            auto pulled = std::async(std::launch::async, [&] { return (*stream)->next(); });
            if(pulled.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                (*stream)->cancel();
            auto batch = pulled.get();
            EXPECT_TRUE(batch.ok()) << batch.status();
            if(!batch.ok() || !*batch)
                return result;
            result.events.insert(result.events.end(), (**batch).events.begin(), (**batch).events.end());
            if((**batch).completion)
            {
                result.completion = (**batch).completion;
                return result;
            }
        }
        ADD_FAILURE() << "tail did not end";
        return result;
    }
    // The switch ran inside the read, every input is gone, and at least one planned input load found its file gone.
    void expectStalePlan()
    {
        EXPECT_EQ(compacted, inputs.size());
        EXPECT_GT(vanished.load(), 0u);
        for(const auto& input: inputs) EXPECT_FALSE(fs::exists(input)) << input;
    }

    fs::path root;
    std::unique_ptr<FileTierStore> writer;
    std::shared_ptr<FileTierStore> archive;
    std::shared_ptr<KeeperModel> source;
    HotReplayOptions options;
    std::vector<fs::path> inputs;
    std::mutex mu;
    std::vector<fs::path> loaded;
    std::atomic<size_t> loads{}, trigger{}, vanished{}, compacted{};
};

TEST_F(CompactedReplay, HlcReadPlannedAgainstInputsMatchesAnUncompactedRead)
{
    for(size_t threads: {1u, 4u})
        for(bool last: {false, true})
            for(size_t limit: {0u, 4u, 9u})
            {
                SCOPED_TRACE(testing::Message() << "threads " << threads << " last " << last << " limit " << limit);
                ASSERT_NO_FATAL_FAILURE(
                        open(std::to_string(threads) + "-" + std::to_string(last) + "-" + std::to_string(limit),
                             threads));
                auto archived = publish(100, 6);
                source->response.archived_below = {400, 0};
                source->response.keepers[0].events = windowEvents(350);
                source->response.keepers[0].events.push_back(event(420, 0));
                source->response.keepers[1].events = {event(450, 0)};
                const Range range{Range::Axis::Hlc, {100, 0}, {1000, 0}};
                HotReplay replay(source, options);
                const auto before = drain(replay.read(1, range, limit));
                ASSERT_TRUE(before.completion);
                if(limit == 0)
                {
                    EXPECT_TRUE(before.completion->complete);
                    EXPECT_EQ(before.events.size(), archived.size() + 2);
                }
                else
                    EXPECT_EQ(before.completion->reason, IncompleteReason::Truncated);
                EXPECT_EQ(compacted.load(), 0u);
                // The switch lands before the first or the last byte load of the plan.
                const size_t at = last ? loads.load() : 1;
                trigger = loads + at;
                const auto stale = drain(replay.read(1, range, limit));
                expectStalePlan();
                expectSame(before, stale);
                if(limit == 0)
                    expectSame(before, drain(replay.read(1, range, limit)));
            }
}

TEST_F(CompactedReplay, PhysicalReadPlannedAgainstInputsMatchesAnUncompactedRead)
{
    for(size_t threads: {1u, 4u})
        for(bool last: {false, true})
            for(size_t limit: {0u, 5u})
            {
                SCOPED_TRACE(testing::Message() << "threads " << threads << " last " << last << " limit " << limit);
                ASSERT_NO_FATAL_FAILURE(
                        open(std::to_string(threads) + "-" + std::to_string(last) + "-" + std::to_string(limit),
                             threads));
                auto archived = publish(100, 6, true);
                source->response.physical_policy = true;
                source->response.archived_below = {400, 0};
                for(auto& keeper: source->response.keepers) keeper.frontier.physical_frontier = 1000;
                source->response.keepers[0].events = {event(420, 0)};
                const Range range{Range::Axis::Physical, {100, 0}, {500, 0}};
                HotReplay replay(source, options);
                const auto before = drain(replay.read(1, range, limit));
                ASSERT_TRUE(before.completion);
                if(limit == 0)
                {
                    EXPECT_NE(before.completion->reason, IncompleteReason::SourceFailed);
                    EXPECT_EQ(before.events.size(), archived.size() + 1);
                }
                else
                    EXPECT_EQ(before.completion->reason, IncompleteReason::Truncated);
                EXPECT_EQ(compacted.load(), 0u);
                // The switch lands before the first or the last byte load of the plan.
                const size_t at = last ? loads.load() : 1;
                trigger = loads + at;
                const auto stale = drain(replay.read(1, range, limit));
                expectStalePlan();
                expectSame(before, stale);
            }
}

TEST_F(CompactedReplay, TailArchiveLoadPlannedAgainstInputsMatchesAnUncompactedTail)
{
    // Trigger 2 with one reader thread lands on the tie-group re-read of the first window's file.
    for(auto [threads, at]: std::vector<std::pair<size_t, size_t>>{{1, 1}, {1, 2}, {1, 4}, {4, 1}})
    {
        SCOPED_TRACE(testing::Message() << "threads " << threads << " trigger " << at);
        ASSERT_NO_FATAL_FAILURE(open(std::to_string(threads) + "-" + std::to_string(at), threads));
        auto archived = publish(100, 6);
        options.read_max_events = 2;
        source->response.archived_below = {400, 0};
        source->response.keepers[0].events = {event(420, 0)};
        source->closed_at = {2000, 0};
        const auto before = tail();
        ASSERT_TRUE(before.completion);
        EXPECT_EQ(before.completion->reason, IncompleteReason::None);
        EXPECT_EQ(before.completion->frontier, (Hlc{2000, 0}));
        ASSERT_EQ(before.events.size(), archived.size() + 1);
        for(size_t i = 0; i < archived.size(); ++i) EXPECT_EQ(before.events[i].id, archived[i].id);
        EXPECT_EQ(compacted.load(), 0u);
        ASSERT_GE(loads.load(), at);
        if(threads == 1)
        {
            std::lock_guard lock(mu);
            ASSERT_GE(loaded.size(), 2u);
            // The first window's file is probed once and then re-read for its whole tie group.
            EXPECT_EQ(loaded[0], loaded[1]);
            EXPECT_EQ(loaded[0], inputs[0]);
        }
        {
            std::lock_guard lock(mu);
            loaded.clear();
        }
        trigger = loads + at;
        const auto stale = tail();
        expectStalePlan();
        expectSame(before, stale);
        if(threads == 1 && at == 2)
        {
            std::lock_guard lock(mu);
            ASSERT_GE(loaded.size(), 2u);
            EXPECT_EQ(loaded[1], inputs[0]);
        }
    }
}

// F4: per-request max_events pages over compacted outputs holding more than max_events / 2 events while the
// Keepers still hold hot copies of them: every page is certified and makes progress, and the pages from any start
// concatenate to the unbounded read from there.
TEST_F(CompactedReplay, PagedHlcReadOverCompactedOutputsWithHotCopiesProgresses)
{
    ASSERT_NO_FATAL_FAILURE(open("paging", 4));
    auto first = publish(100, 8);
    auto result = writer->compactOnce(eager());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->inputs, 8u);
    auto second = publish(500, 8);
    result = writer->compactOnce(eager());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->inputs, 8u);
    auto trailing = publish(900, 1);
    ASSERT_TRUE(archive->refreshNow().ok());
    auto manifest = archive->manifest(1);
    ASSERT_TRUE(manifest.ok()) << manifest.status();
    size_t outputs = 0;
    for(const auto& record: *manifest)
        if(record.state == ManifestState::Published && record.event_count == 32)
            ++outputs;
    ASSERT_EQ(outputs, 2u);
    source->response.archived_below = {950, 0};
    auto& a = source->response.keepers[0].events;
    a = second;
    a.insert(a.end(), trailing.begin(), trailing.end());
    for(int64_t t: {960, 970, 980}) a.push_back(event(t, 0));
    auto& b = source->response.keepers[1].events;
    b.assign(first.begin() + 16, first.end());
    b.push_back(event(975, 0));
    b.push_back(event(975, 1));
    const Hlc end{2000, 0};
    HotReplay replay(source, options);
    for(Hlc start: {Hlc{100, 0}, Hlc{175, 0}, Hlc{260, 0}, Hlc{500, 0}, Hlc{705, 1}, Hlc{925, 0}})
    {
        const auto all = drain(replay.read(1, {Range::Axis::Hlc, start, end}, 0));
        ASSERT_TRUE(all.completion);
        ASSERT_TRUE(all.completion->complete);
        for(size_t target: {2u, 5u, 13u, 32u, 47u, 63u})
        {
            SCOPED_TRACE(testing::Message() << "start " << start.physical_ns << " max_events " << target);
            Hlc from = start;
            std::vector<Event> pages;
            bool done = false;
            for(size_t page = 0; page <= all.events.size() && !done; ++page)
            {
                const auto got = drain(replay.read(1, {Range::Axis::Hlc, from, end}, target));
                ASSERT_TRUE(got.completion);
                done = got.completion->complete;
                const Hlc to = done ? end : got.completion->frontier;
                ASSERT_TRUE(done || got.completion->reason == IncompleteReason::Truncated);
                ASSERT_GT(to, from);
                std::vector<Event> expected;
                for(const auto& e: all.events)
                    if(e.hlc >= from && e.hlc < to)
                        expected.push_back(e);
                ASSERT_EQ(got.events.size(), expected.size());
                for(size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(got.events[i].id, expected[i].id);
                pages.insert(pages.end(), got.events.begin(), got.events.end());
                from = to;
            }
            EXPECT_TRUE(done);
            ASSERT_EQ(pages.size(), all.events.size());
            for(size_t i = 0; i < pages.size(); ++i) EXPECT_EQ(pages[i].id, all.events[i].id);
        }
    }
}

} // namespace
} // namespace chronolog::player
