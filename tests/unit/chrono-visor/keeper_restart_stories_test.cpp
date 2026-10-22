#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <KeeperRegistry.h>

using chronolog::ChronicleName;
using chronolog::RecordingGroup;
using chronolog::storiesToStartOnRegistration;
using chronolog::StoryId;
using chronolog::StoryName;

namespace
{

using StoryList = std::vector<std::tuple<ChronicleName, StoryName, StoryId>>;

} // namespace

// A keeper that registers again after a restart must get pipelines for the
// stories its own recording group records, and only those: the other groups'
// keepers never see events of those stories.
TEST(KeeperRestartStories, OnlyStoriesOfTheKeepersGroupAreListed)
{
    RecordingGroup group_one(1);
    RecordingGroup group_two(2);

    std::map<StoryId, RecordingGroup*> const active{{10, &group_one}, {20, &group_two}, {30, &group_one}};
    std::map<StoryId, std::pair<ChronicleName, StoryName>> const names{{10, {"chronicle_a", "story_1"}},
                                                                       {20, {"chronicle_a", "story_2"}},
                                                                       {30, {"chronicle_b", "story_3"}}};

    EXPECT_EQ(storiesToStartOnRegistration(active, names, 1),
              (StoryList{{"chronicle_a", "story_1", 10}, {"chronicle_b", "story_3", 30}}));
    EXPECT_EQ(storiesToStartOnRegistration(active, names, 2), (StoryList{{"chronicle_a", "story_2", 20}}));
    EXPECT_TRUE(storiesToStartOnRegistration(active, names, 3).empty());
}

// On a fresh deployment no client holds a story yet, so a keeper registering
// for the first time is sent nothing.
TEST(KeeperRestartStories, NothingIsListedWhenNoStoryIsActive)
{
    std::map<StoryId, RecordingGroup*> const active;
    std::map<StoryId, std::pair<ChronicleName, StoryName>> const names;

    EXPECT_TRUE(storiesToStartOnRegistration(active, names, 1).empty());
}

// A released story leaves activeStories. Starting it on the keeper would leave
// a pipeline that no stop notification ever reaches.
TEST(KeeperRestartStories, ReleasedStoryIsNotListed)
{
    RecordingGroup group_one(1);

    std::map<StoryId, RecordingGroup*> const active{{10, &group_one}};
    // the names of story 20 outlived its release: the list follows activeStories
    std::map<StoryId, std::pair<ChronicleName, StoryName>> const names{{10, {"chronicle_a", "story_1"}},
                                                                       {20, {"chronicle_a", "story_2"}}};

    EXPECT_EQ(storiesToStartOnRegistration(active, names, 1), (StoryList{{"chronicle_a", "story_1", 10}}));
}

// The keeper's start_story_recording needs the names, so a story the visor
// has no names for cannot be started, and a story with no group is not
// recorded by anyone.
TEST(KeeperRestartStories, StoryWithoutNamesOrGroupIsNotListed)
{
    RecordingGroup group_one(1);

    std::map<StoryId, RecordingGroup*> const active{{10, &group_one}, {20, &group_one}, {30, nullptr}};
    std::map<StoryId, std::pair<ChronicleName, StoryName>> const names{{10, {"chronicle_a", "story_1"}},
                                                                       {30, {"chronicle_a", "story_3"}}};

    EXPECT_EQ(storiesToStartOnRegistration(active, names, 1), (StoryList{{"chronicle_a", "story_1", 10}}));
}
