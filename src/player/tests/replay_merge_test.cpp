#include <gtest/gtest.h>
#include "chrono-player/replay/ReplayMerge.h"

namespace chronolog::player
{
namespace
{

Event ev(uint64_t writer, uint64_t sequence, int64_t hlc, Durability d = Durability::Accepted)
{
    Event e;
    e.id = {1, writer, 1, sequence};
    e.hlc = {hlc, 0};
    e.durability = d;
    return e;
}

TEST(ReplayMerge, MergesInReplayOrder)
{
    auto out = mergeReplay({{ev(2, 1, 10), ev(2, 2, 30)}, {ev(4, 1, 20), ev(4, 2, 40)}, {}});
    ASSERT_EQ(out.size(), 4u);
    for(size_t i = 1; i < out.size(); ++i) EXPECT_TRUE(ReplayLess(out[i - 1], out[i]));
}

TEST(ReplayMerge, EventFromTheArchiveAndAKeeperIsReturnedOnce)
{
    auto out = mergeReplay({{ev(2, 1, 10, Durability::Durable), ev(2, 2, 20)}, {ev(2, 1, 10), ev(2, 2, 20)}});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].durability, Durability::Durable);
}

TEST(ReplayMerge, DuplicateKeepsHighestDurability)
{
    auto out = mergeReplay({{ev(2, 1, 10)}, {ev(2, 1, 10, Durability::Durable)}});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].durability, Durability::Durable);
}

TEST(ReplayMerge, EventsDifferingOnlyInWriterIdAreDifferentEvents)
{
    auto out = mergeReplay({{ev(2, 1, 10)}, {ev(4, 1, 10)}});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].id.writer_id, 2u);
    EXPECT_EQ(out[1].id.writer_id, 4u);
}

TEST(ReplayMerge, EventsDifferingOnlyInIncarnationAreDifferentEvents)
{
    Event a = ev(2, 1, 10), b = ev(2, 1, 10);
    b.id.incarnation = 2;
    EXPECT_EQ(mergeReplay({{a}, {b}}).size(), 2u);
}

TEST(ReplayMerge, EmptyInputs)
{
    EXPECT_TRUE(mergeReplay({}).empty());
    EXPECT_TRUE(mergeReplay({{}, {}}).empty());
}

} // namespace
} // namespace chronolog::player
