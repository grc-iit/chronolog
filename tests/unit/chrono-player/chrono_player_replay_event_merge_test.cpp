// Unit tests for mergeReplayEvents: the player's merge of the events a replay
// read from the archive with the events the story's keepers served. Nothing
// downstream removes duplicates, so an event that arrives twice has to leave
// the merge once, and the series has to stay in time order.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <ReplayEventMerge.h>

namespace chl = chronolog;

namespace
{
chl::Event event(uint64_t time, chl::ClientId client, chl::chrono_index index)
{
    return chl::Event{time, client, index, "event@" + std::to_string(time)};
}

std::vector<uint64_t> eventTimes(std::vector<chl::Event> const& events)
{
    std::vector<uint64_t> times;
    for(auto const& replayed: events) { times.push_back(replayed.time()); }
    return times;
}
} // namespace

TEST(ReplayEventMerge, EventFromTheArchiveAndAKeeperIsReturnedOnce)
{
    // the grapher wrote the keeper's chunk, but the keeper had not heard yet
    auto merged = chl::mergeReplayEvents({event(100, 1, 0), event(110, 1, 1)}, {event(110, 1, 1), event(120, 1, 2)});
    EXPECT_EQ(eventTimes(merged), (std::vector<uint64_t>{100, 110, 120}));
}

TEST(ReplayEventMerge, EventWrittenTwiceToTheArchiveIsReturnedOnce)
{
    // a re-sent chunk was written to a second file
    auto merged = chl::mergeReplayEvents({event(100, 1, 0), event(100, 1, 0), event(110, 1, 1)}, {});
    EXPECT_EQ(eventTimes(merged), (std::vector<uint64_t>{100, 110}));
}

TEST(ReplayEventMerge, SeriesIsAscendingWhenKeeperEventsFallBetweenArchivedOnes)
{
    // a keeper serves events the grapher has not confirmed, which can be older
    // than the newest archived ones
    auto merged = chl::mergeReplayEvents({event(100, 1, 0), event(130, 1, 3)}, {event(110, 2, 0), event(140, 1, 4)});
    EXPECT_EQ(eventTimes(merged), (std::vector<uint64_t>{100, 110, 130, 140}));
}

TEST(ReplayEventMerge, EventsDifferingOnlyInClientIdAreDifferentEvents)
{
    auto merged = chl::mergeReplayEvents({event(100, 1, 0)}, {event(100, 2, 0)});
    EXPECT_EQ(merged.size(), 2u);
}
