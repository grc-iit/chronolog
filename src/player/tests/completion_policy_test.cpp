#include <gtest/gtest.h>
#include "player/replay/CompletionPolicy.h"

namespace chronolog::player
{
namespace
{

constexpr Epoch kEpoch = 7;

Range hlcRange() { return {Range::Axis::Hlc, {100, 0}, {300, 0}}; }

KeeperFrontier keeper(std::string id, int64_t seal, bool answered = true, Epoch epoch = kEpoch)
{
    KeeperFrontier k;
    k.process_id = std::move(id);
    k.epoch = epoch;
    k.sealed = {seal, 0};
    k.answered = answered;
    return k;
}

const std::vector<WriterAssignment> kWriters{{2, 3, "a"}, {4, 3, "b"}};

TEST(CompletionPolicy, CompleteWhenEverySealReachesEnd)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 301), keeper("b", 300)}, kWriters);
    EXPECT_TRUE(c.complete);
    EXPECT_EQ(c.reason, IncompleteReason::None);
    EXPECT_TRUE(c.laggards.empty());
    EXPECT_EQ(c.frontier, (Hlc{300, 0}));
}

TEST(CompletionPolicy, LaggardsAreWritersOnLaggingKeepers)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 301), keeper("b", 299)}, kWriters);
    EXPECT_FALSE(c.complete);
    EXPECT_EQ(c.reason, IncompleteReason::LaggingWriters);
    ASSERT_EQ(c.laggards.size(), 1u);
    EXPECT_EQ(c.laggards[0].writer_id, 4u);
    EXPECT_EQ(c.laggards[0].frontier, (Hlc{299, 0}));
    EXPECT_EQ(c.frontier, (Hlc{299, 0}));
}

TEST(CompletionPolicy, AKeeperThatDidNotAnswerIsSourceFailed)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 400), keeper("b", 400, false)}, kWriters);
    EXPECT_FALSE(c.complete);
    EXPECT_EQ(c.reason, IncompleteReason::SourceFailed);
    ASSERT_EQ(c.laggards.size(), 1u);
    EXPECT_EQ(c.laggards[0].writer_id, 4u);
    EXPECT_EQ(c.frontier, (Hlc{400, 0}));
}

TEST(CompletionPolicy, FailedKeeperWithNoKnownWriterIsStillSourceFailed)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 400), keeper("b", 400, false)}, {});
    EXPECT_EQ(c.reason, IncompleteReason::SourceFailed);
    EXPECT_TRUE(c.laggards.empty());
}

TEST(CompletionPolicy, EpochMismatchIsSourceFailed)
{
    auto c = CompletionPolicy::decide(hlcRange(),
                                      kEpoch,
                                      {keeper("a", 400), keeper("b", 400, true, kEpoch - 1)},
                                      kWriters);
    EXPECT_EQ(c.reason, IncompleteReason::SourceFailed);
}

TEST(CompletionPolicy, TruncatedBeatsLaggingButNotFailure)
{
    auto a = keeper("a", 100);
    a.truncated = true;
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {a, keeper("b", 400)}, kWriters);
    EXPECT_EQ(c.reason, IncompleteReason::Truncated);
    c = CompletionPolicy::decide(hlcRange(), kEpoch, {a, keeper("b", 400, false)}, kWriters);
    EXPECT_EQ(c.reason, IncompleteReason::SourceFailed);
}

TEST(CompletionPolicy, PhysicalAxisIsAlwaysUnbounded)
{
    auto r = hlcRange();
    r.axis = Range::Axis::Physical;
    auto c = CompletionPolicy::decide(r, kEpoch, {keeper("a", 400), keeper("b", 400)}, kWriters);
    EXPECT_FALSE(c.complete);
    EXPECT_EQ(c.reason, IncompleteReason::PhysicalAxisUnbounded);
    EXPECT_TRUE(c.laggards.empty());
}

TEST(CompletionPolicy, WriterOnAKeeperOutsideTheRouteIsIgnored)
{
    auto w = kWriters;
    w.push_back({9, 1, "elsewhere"});
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 300), keeper("b", 300)}, w);
    EXPECT_TRUE(c.complete);
    EXPECT_TRUE(c.laggards.empty());
}

TEST(CompletionPolicy, UnknownWriterIsCoveredByItsKeeperSeal)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {keeper("a", 300), keeper("b", 300)}, {});
    EXPECT_TRUE(c.complete);
}

TEST(CompletionPolicy, EmptyRouteIsSourceFailed)
{
    auto c = CompletionPolicy::decide(hlcRange(), kEpoch, {}, {});
    EXPECT_FALSE(c.complete);
    EXPECT_EQ(c.reason, IncompleteReason::SourceFailed);
}

} // namespace
} // namespace chronolog::player
