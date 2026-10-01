#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "chronolog/types.h"

namespace
{

chronolog::Event makeEvent(int64_t physical, uint32_t logical, uint64_t writer, uint64_t incarnation, uint64_t sequence)
{
    chronolog::Event e;
    e.id.story_id = 1;
    e.id.writer_id = writer;
    e.id.incarnation = incarnation;
    e.id.sequence = sequence;
    e.hlc.physical_ns = physical;
    e.hlc.logical = logical;
    return e;
}

TEST(Types, ReplayLessOrdersByHlcThenIdentity)
{
    std::vector<chronolog::Event> events = {makeEvent(10, 0, 2, 1, 1),
                                            makeEvent(10, 0, 1, 2, 1),
                                            makeEvent(10, 0, 1, 1, 2),
                                            makeEvent(10, 0, 1, 1, 1),
                                            makeEvent(9, 5, 9, 9, 9),
                                            makeEvent(10, 1, 0, 0, 0)};
    std::sort(events.begin(), events.end(), chronolog::ReplayLess);
    EXPECT_EQ(events[0].hlc.physical_ns, 9);
    EXPECT_EQ(events[1].id.sequence, 1u);
    EXPECT_EQ(events[1].id.writer_id, 1u);
    EXPECT_EQ(events[1].id.incarnation, 1u);
    EXPECT_EQ(events[2].id.sequence, 2u);
    EXPECT_EQ(events[3].id.incarnation, 2u);
    EXPECT_EQ(events[4].id.writer_id, 2u);
    EXPECT_EQ(events[5].hlc.logical, 1u);
}

TEST(Types, HlcComparesPhysicalBeforeLogical)
{
    EXPECT_LT((chronolog::Hlc{1, 9}), (chronolog::Hlc{2, 0}));
    EXPECT_LT((chronolog::Hlc{2, 0}), (chronolog::Hlc{2, 1}));
    EXPECT_EQ((chronolog::Hlc{3, 3}), (chronolog::Hlc{3, 3}));
}

} // namespace
