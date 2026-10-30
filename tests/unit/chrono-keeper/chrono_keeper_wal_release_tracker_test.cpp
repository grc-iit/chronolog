// Unit tests for WalReleaseTracker, the WAL's record of which durable events
// are still pending and in which segments, which decides when a segment can
// be deleted.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <StoryChunk.h>
#include <WalReleaseTracker.h>

namespace chl = chronolog;

namespace
{

constexpr uint64_t kChunkStart = 1000;
constexpr uint64_t kChunkEnd = 2000;

chl::EventSequence seq(uint64_t time, uint64_t client = 7, uint32_t index = 0) { return {time, client, index}; }

// A chunk of `story` over [kChunkStart, kChunkEnd) holding the given events.
chl::StoryChunk makeChunk(chl::StoryId story, std::vector<chl::EventSequence> const& events)
{
    chl::StoryChunk chunk("chronicle", "story", story, kChunkStart, kChunkEnd);
    for(auto const& event: events)
    {
        chunk.insertEvent(chl::LogEvent(story, std::get<0>(event), std::get<1>(event), std::get<2>(event), "payload"));
    }
    return chunk;
}

TEST(WalReleaseTracker, CopiesInTwoSegmentsReleasedByOneChunk)
{
    chl::WalReleaseTracker tracker;
    tracker.addCopy(1, seq(1100), 1, 64);
    tracker.addCopy(1, seq(1200), 1, 80);
    // a retry after a crash wrote the first event again in the next segment
    tracker.addCopy(1, seq(1100), 2, 64);

    EXPECT_EQ(tracker.pendingEvents(), 2u);
    EXPECT_EQ(tracker.pendingEvents(1), 2u);
    EXPECT_EQ(tracker.pendingBytes(), 208u);
    EXPECT_EQ(tracker.outstanding(1), 2u);
    EXPECT_EQ(tracker.outstanding(2), 1u);

    tracker.releaseChunk(1, makeChunk(1, {seq(1100), seq(1200)}));

    EXPECT_FALSE(tracker.isPending(1, seq(1100)));
    EXPECT_FALSE(tracker.isPending(1, seq(1200)));
    EXPECT_EQ(tracker.pendingEvents(), 0u);
    EXPECT_EQ(tracker.pendingEvents(1), 0u);
    EXPECT_EQ(tracker.pendingBytes(), 0u);
    EXPECT_EQ(tracker.outstanding(1), 0u);
    EXPECT_EQ(tracker.outstanding(2), 0u);
    EXPECT_EQ(tracker.deletable({1, 2, 3}, 3), (std::vector<uint64_t>{1, 2}));
}

TEST(WalReleaseTracker, ChunkReleasesOnlyItsOwnEvents)
{
    chl::WalReleaseTracker tracker;
    tracker.addCopy(1, seq(1100), 1, 10);
    tracker.addCopy(1, seq(1150, 8), 1, 10); // same range, another client, not in the chunk
    tracker.addCopy(1, seq(1300), 1, 10);
    tracker.addCopy(1, seq(2500), 1, 10); // a later chunk's event

    // more chunk events than pending ones: the walk goes over the pending side
    tracker.releaseChunk(1, makeChunk(1, {seq(1100), seq(1300), seq(1400), seq(1500), seq(1600)}));

    EXPECT_FALSE(tracker.isPending(1, seq(1100)));
    EXPECT_FALSE(tracker.isPending(1, seq(1300)));
    EXPECT_TRUE(tracker.isPending(1, seq(1150, 8)));
    EXPECT_TRUE(tracker.isPending(1, seq(2500)));
    EXPECT_EQ(tracker.pendingEvents(1), 2u);
    EXPECT_EQ(tracker.pendingBytes(), 20u);
    EXPECT_EQ(tracker.outstanding(1), 2u);
}

TEST(WalReleaseTracker, ChunkWithFewerEventsThanPendingReleasesThem)
{
    // fewer chunk events than pending ones: the walk goes over the chunk side
    chl::WalReleaseTracker tracker;
    for(uint32_t i = 0; i < 100; ++i) { tracker.addCopy(1, seq(1000 + i * 10), 1, 1); }

    tracker.releaseChunk(1, makeChunk(1, {seq(1100), seq(1500)}));

    EXPECT_FALSE(tracker.isPending(1, seq(1100)));
    EXPECT_FALSE(tracker.isPending(1, seq(1500)));
    EXPECT_TRUE(tracker.isPending(1, seq(1110)));
    EXPECT_EQ(tracker.pendingEvents(1), 98u);
    EXPECT_EQ(tracker.outstanding(1), 98u);
}

TEST(WalReleaseTracker, ReleaseOfUnknownIdentityIsANoOp)
{
    chl::WalReleaseTracker tracker;
    tracker.releaseEvent(1, seq(1100)); // nothing tracked at all
    tracker.releaseChunk(1, makeChunk(1, {seq(1100)}));
    tracker.dropStory(1);
    EXPECT_EQ(tracker.pendingEvents(), 0u);

    tracker.addCopy(1, seq(1100), 1, 50);
    tracker.releaseEvent(1, seq(1101));       // other time
    tracker.releaseEvent(1, seq(1100, 8));    // other client
    tracker.releaseEvent(1, seq(1100, 7, 1)); // other index
    tracker.releaseEvent(2, seq(1100));       // other story

    EXPECT_TRUE(tracker.isPending(1, seq(1100)));
    EXPECT_FALSE(tracker.isPending(2, seq(1100)));
    EXPECT_EQ(tracker.pendingEvents(), 1u);
    EXPECT_EQ(tracker.pendingBytes(), 50u);
    EXPECT_EQ(tracker.outstanding(1), 1u);

    tracker.releaseEvent(1, seq(1100));
    tracker.releaseEvent(1, seq(1100)); // twice
    EXPECT_EQ(tracker.pendingEvents(), 0u);
    EXPECT_EQ(tracker.pendingBytes(), 0u);
    EXPECT_EQ(tracker.outstanding(1), 0u);
}

TEST(WalReleaseTracker, DeletableStopsAtFirstOutstandingSegment)
{
    chl::WalReleaseTracker tracker;
    tracker.addCopy(1, seq(1100), 1, 10);
    tracker.addCopy(1, seq(1200), 3, 10);
    tracker.addCopy(2, seq(1300), 5, 10);

    // segment 1 pins everything after it, even the empty segment 2
    EXPECT_TRUE(tracker.deletable({1, 2, 3, 4, 5, 6}, 6).empty());

    tracker.releaseEvent(1, seq(1100));
    EXPECT_EQ(tracker.deletable({1, 2, 3, 4, 5, 6}, 6), (std::vector<uint64_t>{1, 2}));

    tracker.releaseEvent(1, seq(1200));
    EXPECT_EQ(tracker.deletable({1, 2, 3, 4, 5, 6}, 6), (std::vector<uint64_t>{1, 2, 3, 4}));

    // the list may start past segments already deleted
    EXPECT_EQ(tracker.deletable({3, 4, 5, 6}, 6), (std::vector<uint64_t>{3, 4}));
    EXPECT_TRUE(tracker.deletable({}, 6).empty());
}

TEST(WalReleaseTracker, DeletableNeverIncludesActive)
{
    chl::WalReleaseTracker tracker;
    EXPECT_EQ(tracker.deletable({1, 2, 3}, 3), (std::vector<uint64_t>{1, 2}));
    EXPECT_TRUE(tracker.deletable({1}, 1).empty());
    // stops at the active segment even when later ones are empty too
    EXPECT_EQ(tracker.deletable({1, 2, 3, 4}, 2), (std::vector<uint64_t>{1}));

    tracker.addCopy(1, seq(1100), 2, 10);
    tracker.releaseEvent(1, seq(1100));
    EXPECT_EQ(tracker.deletable({1, 2, 3}, 2), (std::vector<uint64_t>{1}));
}

TEST(WalReleaseTracker, DropStoryKeepsCopiesAddedAfterIt)
{
    chl::WalReleaseTracker tracker;
    tracker.addCopy(1, seq(1100), 1, 10);
    tracker.addCopy(1, seq(1200), 1, 10);
    tracker.addCopy(2, seq(1100), 1, 10); // another story keeps its copy

    tracker.dropStory(1);
    EXPECT_EQ(tracker.pendingEvents(1), 0u);
    EXPECT_EQ(tracker.outstanding(1), 1u);

    // the story re-created under the same id; its copy is in the next segment
    tracker.addCopy(1, seq(1100), 2, 30);

    EXPECT_EQ(tracker.pendingEvents(1), 1u);
    EXPECT_TRUE(tracker.isPending(1, seq(1100)));
    EXPECT_FALSE(tracker.isPending(1, seq(1200)));
    EXPECT_TRUE(tracker.isPending(2, seq(1100)));
    EXPECT_EQ(tracker.pendingEvents(), 2u);
    EXPECT_EQ(tracker.pendingBytes(), 40u);
    EXPECT_EQ(tracker.outstanding(1), 1u);
    EXPECT_EQ(tracker.outstanding(2), 1u);
}

TEST(WalReleaseTracker, ReleaseChunkSkipsStoriesWithNothingPending)
{
    chl::WalReleaseTracker tracker;
    tracker.addCopy(1, seq(1100), 1, 10);

    // a large chunk of a story with no durable events, holding the same
    // identities as story 1's pending event
    std::vector<chl::EventSequence> events;
    for(uint64_t t = kChunkStart; t < kChunkEnd; ++t)
    {
        for(uint32_t i = 0; i < 100; ++i) { events.push_back(seq(t, 7, i)); }
    }
    chl::StoryChunk chunk = makeChunk(2, events);
    ASSERT_EQ(chunk.getEventCount(), 100000);

    auto const started = std::chrono::steady_clock::now();
    for(int i = 0; i < 10000; ++i) { tracker.releaseChunk(2, chunk); }
    auto const elapsed = std::chrono::steady_clock::now() - started;

    // walking the chunk each time would be 10^9 lookups
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_TRUE(tracker.isPending(1, seq(1100)));
    EXPECT_EQ(tracker.pendingEvents(), 1u);
    EXPECT_EQ(tracker.outstanding(1), 1u);

    // a story whose last pending event was released is skipped the same way
    tracker.releaseEvent(1, seq(1100));
    tracker.releaseChunk(1, chunk);
    EXPECT_EQ(tracker.pendingEvents(), 0u);
}

TEST(WalReleaseTracker, TenThousandStoriesReleaseInLinearTime)
{
    constexpr uint64_t kStories = 10000;
    constexpr uint32_t kEventsPerStory = 3;
    constexpr uint64_t kCopiesPerSegment = 100;

    chl::WalReleaseTracker tracker;
    std::vector<chl::StoryChunk> chunks;
    chunks.reserve(kStories);
    for(uint64_t story = 1; story <= kStories; ++story)
    {
        chunks.emplace_back("c", "s", story, kChunkStart, kChunkEnd);
    }

    // events of every story interleaved in log order, so each story spans
    // several of the 300 segments and each segment holds many stories
    auto const started = std::chrono::steady_clock::now();
    uint64_t copies = 0;
    for(uint32_t i = 0; i < kEventsPerStory; ++i)
    {
        for(uint64_t story = 1; story <= kStories; ++story)
        {
            chl::EventSequence const id = seq(kChunkStart + 100 * i, story, i);
            tracker.addCopy(story, id, 1 + copies / kCopiesPerSegment, 100);
            chunks[story - 1].insertEvent(chl::LogEvent(story, std::get<0>(id), std::get<1>(id), i, "payload"));
            ++copies;
        }
    }
    uint64_t const segments = copies / kCopiesPerSegment;
    ASSERT_EQ(segments, 300u);
    EXPECT_EQ(tracker.pendingEvents(), kStories * kEventsPerStory);
    EXPECT_EQ(tracker.pendingBytes(), kStories * kEventsPerStory * 100);

    std::vector<uint64_t> all_segments;
    for(uint64_t segment = 1; segment <= segments + 1; ++segment) { all_segments.push_back(segment); }
    EXPECT_TRUE(tracker.deletable(all_segments, segments + 1).empty());

    for(uint64_t story = 1; story <= kStories; ++story) { tracker.releaseChunk(story, chunks[story - 1]); }
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(tracker.pendingEvents(), 0u);
    EXPECT_EQ(tracker.pendingBytes(), 0u);
    for(uint64_t segment = 1; segment <= segments; ++segment) { EXPECT_EQ(tracker.outstanding(segment), 0u); }
    all_segments.pop_back();
    EXPECT_EQ(tracker.deletable(all_segments, segments + 1), all_segments);
}

} // namespace
