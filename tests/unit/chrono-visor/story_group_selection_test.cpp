#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <StoryGroupSelection.h>

using chronolog::add_active_group;
using chronolog::RecordingGroupId;
using chronolog::remove_active_group;
using chronolog::select_recording_group;

namespace
{

constexpr uint64_t kStories = 20000;

// Story ids 0, 1, 2, ...: consecutive ids are the hardest case for the
// hashing, since real ids are already CityHash values.
std::map<uint64_t, RecordingGroupId> assign_all(std::vector<RecordingGroupId> const& groups)
{
    std::map<uint64_t, RecordingGroupId> assignment;
    for(uint64_t story = 0; story < kStories; ++story)
    {
        std::optional<RecordingGroupId> group = select_recording_group(story, groups);
        EXPECT_TRUE(group.has_value());
        assignment[story] = group.value_or(0);
    }
    return assignment;
}

} // namespace

// A story released and acquired again must go back to the group whose keepers
// still hold its recent events (#706).
TEST(StoryGroupSelection, SameStoryGetsTheSameGroupEveryTime)
{
    std::vector<RecordingGroupId> const groups{1, 2};
    for(uint64_t story: {0ULL, 1ULL, 4433715426784025336ULL, 0xFFFFFFFFFFFFFFFFULL})
    {
        std::optional<RecordingGroupId> first = select_recording_group(story, groups);
        ASSERT_TRUE(first.has_value());
        for(int i = 0; i < 100; ++i) { EXPECT_EQ(select_recording_group(story, groups), first); }
    }
}

// The visor lists its active groups in the order they registered, which can
// differ from one start to the next.
TEST(StoryGroupSelection, OrderOfTheGroupsDoesNotMatter)
{
    std::map<uint64_t, RecordingGroupId> const forward = assign_all({1, 2, 3, 4});
    EXPECT_EQ(assign_all({4, 2, 3, 1}), forward);
    EXPECT_EQ(assign_all({3, 1, 4, 2}), forward);
}

TEST(StoryGroupSelection, OnlyStoriesOfARemovedGroupMove)
{
    std::map<uint64_t, RecordingGroupId> const before = assign_all({1, 2, 3, 4});
    std::map<uint64_t, RecordingGroupId> const after = assign_all({1, 2, 4});
    for(auto const& [story, group]: before)
    {
        if(group == 3)
        {
            EXPECT_NE(after.at(story), 3u) << "story " << story;
        }
        else
        {
            EXPECT_EQ(after.at(story), group) << "story " << story;
        }
    }
}

TEST(StoryGroupSelection, StoriesMoveOnlyToAnAddedGroup)
{
    std::map<uint64_t, RecordingGroupId> const before = assign_all({1, 2, 3});
    std::map<uint64_t, RecordingGroupId> const after = assign_all({1, 2, 3, 7});
    size_t moved = 0;
    for(auto const& [story, group]: before)
    {
        if(after.at(story) != group)
        {
            EXPECT_EQ(after.at(story), 7u) << "story " << story;
            ++moved;
        }
    }
    // the new group takes about a quarter of the stories
    EXPECT_GT(moved, kStories / 4 * 9 / 10);
    EXPECT_LT(moved, kStories / 4 * 11 / 10);
}

TEST(StoryGroupSelection, StoriesSpreadEvenlyOverTheGroups)
{
    for(std::vector<RecordingGroupId> const& groups:
        std::vector<std::vector<RecordingGroupId>>{{1, 2}, {1, 2, 3, 4}, {10, 20, 30, 40, 50}})
    {
        std::map<RecordingGroupId, size_t> count;
        for(auto const& [story, group]: assign_all(groups)) { ++count[group]; }
        uint64_t const fair = kStories / groups.size();
        for(RecordingGroupId group: groups)
        {
            EXPECT_GT(count[group], fair * 9 / 10) << "group " << group << " of " << groups.size();
            EXPECT_LT(count[group], fair * 11 / 10) << "group " << group << " of " << groups.size();
        }
    }
}

TEST(StoryGroupSelection, OneGroupTakesEveryStory)
{
    for(uint64_t story = 0; story < 100; ++story) { EXPECT_EQ(select_recording_group(story, {5}), 5u); }
}

TEST(StoryGroupSelection, NoGroupNoSelection) { EXPECT_FALSE(select_recording_group(42, {}).has_value()); }

// A group listed twice in the visor's active groups stays listed after one
// removal, and keeps receiving the stories that hash to it with no grapher or
// no keepers left.
TEST(ActiveGroups, AGroupIsListedOnce)
{
    int a = 0, b = 0;
    std::vector<int*> groups;
    add_active_group(groups, &a);
    add_active_group(groups, &b);
    add_active_group(groups, &a);
    EXPECT_EQ(groups, (std::vector<int*>{&a, &b}));
}

TEST(ActiveGroups, RemovingAGroupRemovesEveryCopy)
{
    int a = 0, b = 0, c = 0;
    std::vector<int*> groups{&a, &b, &c, &b};
    remove_active_group(groups, &b);
    EXPECT_EQ(groups, (std::vector<int*>{&a, &c}));
}

TEST(ActiveGroups, RemovingAGroupThatIsNotListedChangesNothing)
{
    int a = 0, b = 0, c = 0;
    std::vector<int*> groups{&a, &b};
    remove_active_group(groups, &c);
    EXPECT_EQ(groups, (std::vector<int*>{&a, &b}));
    std::vector<int*> none;
    remove_active_group(none, &c);
    EXPECT_TRUE(none.empty());
}
