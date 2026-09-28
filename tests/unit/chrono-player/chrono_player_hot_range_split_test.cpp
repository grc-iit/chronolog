// Unit tests for splitHotRange: where a replay of [start, end) splits between
// the archive and the story's keepers. The archive serves [start, B) and the
// keepers [B, end). A keeper frees only chunks its known watermark covers, so
// B has to be the highest watermark any keeper reports: a lower B leaves the
// events some keeper freed above it served by nobody. With no watermark
// reported anywhere nothing has been freed, and B is the lowest hot floor.
// Events a keeper holds but the grapher never acknowledged are served
// whatever B is, since the archive cannot have them.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <HotRangeSplit.h>

namespace chl = chronolog;

namespace
{
constexpr chl::StoryId kStory = 9;
constexpr uint64_t kStart = 100;
constexpr uint64_t kEnd = 300;

// What one keeper's story_range_fetch returns: its oldest retained tick and
// the events it retains in range.
chl::HotRangeResponse keeperResponse(uint64_t hot_floor, std::vector<uint64_t> const& event_times)
{
    chl::HotRangeResponse response;
    response.hot_floor = hot_floor;
    for(uint64_t time: event_times)
    {
        response.events.emplace_back(kStory, time, 1, 0, "event@" + std::to_string(time));
    }
    return response;
}

std::vector<uint64_t> eventTimes(chl::HotRangeSplit const& split)
{
    std::vector<uint64_t> times;
    for(auto const& event: split.hotEvents) { times.push_back(event.time()); }
    return times;
}
} // namespace

TEST(HotRangeSplit, NoKeepersIsArchiveOnly)
{
    std::vector<chl::HotRangeResponse> responses;
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, kEnd);
    EXPECT_TRUE(split.hotEvents.empty());
    EXPECT_FALSE(split.complete);
}

TEST(HotRangeSplit, SingleKeeperSplitsAtItsFloor)
{
    std::vector<chl::HotRangeResponse> responses{keeperResponse(150, {150, 160, 170})};
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, 150u);
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{150, 160, 170}));
    EXPECT_FALSE(split.complete);
}

TEST(HotRangeSplit, BoundaryIsTheMinimumFloorAcrossKeepers)
{
    // three keepers holding stripes of the story, each freed to its own floor
    std::vector<chl::HotRangeResponse> responses{keeperResponse(210, {210, 220}),
                                                 keeperResponse(180, {180, 190, 230}),
                                                 keeperResponse(240, {240})};
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    // 180, not 210 or 240: those would drop [180, 210) held only by the second keeper
    EXPECT_EQ(split.boundary, 180u);
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{180, 190, 210, 220, 230, 240}));
    EXPECT_FALSE(split.complete);
}

TEST(HotRangeSplit, KeeperHoldingNothingAndFailedFetchDropOutOfTheMin)
{
    std::vector<chl::HotRangeResponse> responses{
            keeperResponse(150, {150}),
            keeperResponse(UINT64_MAX, {}), // retains nothing for the story
            chl::HotRangeResponse{}};       // what KeeperHotFetchClient returns when the RPC fails
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, 150u);
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{150}));
}

TEST(HotRangeSplit, HotSideReachingStartIsComplete)
{
    std::vector<chl::HotRangeResponse> responses{keeperResponse(kStart, {kStart, 120})};
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, kStart);
    EXPECT_TRUE(split.complete);
}

TEST(HotRangeSplit, EventHeldByTwoKeepersIsReturnedOnce)
{
    std::vector<chl::HotRangeResponse> responses{keeperResponse(150, {150, 160}), keeperResponse(150, {150})};
    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{150, 160}));
}

TEST(HotRangeSplit, BoundaryIsTheHighestWatermarkAKeeperReports)
{
    // keeper A still holds an old chunk at 100 (its events are still in the
    // tail index) and knows W = 200; keeper B, with W = 180, already freed
    // everything it had below 210. A split at A's floor (100) would leave B's
    // freed events in [100, 200) to an archive read that stops at 100.
    chl::HotRangeResponse keeper_a = keeperResponse(100, {100, 105, 250});
    keeper_a.known_W = 200;
    chl::HotRangeResponse keeper_b = keeperResponse(210, {210, 220});
    keeper_b.known_W = 180;
    std::vector<chl::HotRangeResponse> responses{keeper_a, keeper_b};

    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, 200u);
    // 100 and 105 are below W and in the archive; keeping them would duplicate them
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{210, 220, 250}));
    EXPECT_FALSE(split.complete);
}

TEST(HotRangeSplit, UnacknowledgedEventsBelowTheBoundaryAreKept)
{
    // keeper A's chunk at 120 never reached the grapher, while other keepers'
    // data moved W to 200: the archive does not have 120 and 130
    chl::HotRangeResponse keeper_a;
    keeper_a.hot_floor = 120;
    keeper_a.known_W = 200;
    keeper_a.unconfirmed_events.emplace_back(kStory, 120, 1, 0, "event@120");
    keeper_a.unconfirmed_events.emplace_back(kStory, 130, 1, 1, "event@130");
    chl::HotRangeResponse keeper_b = keeperResponse(210, {210});
    keeper_b.known_W = 200;
    std::vector<chl::HotRangeResponse> responses{keeper_a, keeper_b};

    auto split = chl::splitHotRange(responses, kStart, kEnd);
    EXPECT_EQ(split.boundary, 200u);
    EXPECT_EQ(eventTimes(split), (std::vector<uint64_t>{120, 130, 210}));
}
