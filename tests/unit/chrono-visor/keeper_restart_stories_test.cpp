#include <map>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <VisorAdminRuntime.h>

using chronolog::AdminCompletion;
using chronolog::storiesToStartOnRegistration;
using chronolog::StoryId;
using chronolog::StoryRecording;
using chronolog::StoryStopTargets;
using chronolog::storyStopTargets;

namespace
{

std::shared_ptr<StoryRecording> recording(StoryId id, chronolog::RecordingGroupId group, StoryRecording::Phase phase)
{
    auto r = std::make_shared<StoryRecording>();
    r->id = id;
    r->group = group;
    r->phase = phase;
    return r;
}

std::vector<StoryId> ids(std::vector<std::shared_ptr<StoryRecording>> const& stories)
{
    std::vector<StoryId> out;
    for(auto const& s: stories) out.push_back(s->id);
    return out;
}

std::shared_ptr<AdminCompletion> settled(int status, bool confirmed = true)
{
    auto c = std::make_shared<AdminCompletion>();
    c->status = status;
    c->confirmed = confirmed;
    c->done.store(true);
    return c;
}

} // namespace

// A keeper that registers again after a restart must get pipelines for the
// stories its own recording group records, and only those: the other groups'
// keepers never see events of those stories.
TEST(KeeperRestartStories, OnlyStoriesOfTheKeepersGroupAreListed)
{
    std::map<StoryId, std::shared_ptr<StoryRecording>> const active{{10, recording(10, 1, StoryRecording::Active)},
                                                                    {20, recording(20, 2, StoryRecording::Active)},
                                                                    {30, recording(30, 1, StoryRecording::Active)}};

    EXPECT_EQ(ids(storiesToStartOnRegistration(active, 1)), (std::vector<StoryId>{10, 30}));
    EXPECT_EQ(ids(storiesToStartOnRegistration(active, 2)), (std::vector<StoryId>{20}));
    EXPECT_TRUE(storiesToStartOnRegistration(active, 3).empty());
}

// On a fresh deployment no client holds a story yet, so a keeper registering
// for the first time is sent nothing.
TEST(KeeperRestartStories, NothingIsListedWhenNoStoryIsActive)
{
    std::map<StoryId, std::shared_ptr<StoryRecording>> const active;
    EXPECT_TRUE(storiesToStartOnRegistration(active, 1).empty());
}

// A story still being acquired, being stopped, or left unresolved by a failed
// acquire or stop is not started: its first acquire or its stop owns it, and a
// start sent now could outlive the stop that ends it.
TEST(KeeperRestartStories, OnlyActiveStoriesAreListed)
{
    std::map<StoryId, std::shared_ptr<StoryRecording>> const active{{10, recording(10, 1, StoryRecording::Starting)},
                                                                    {20, recording(20, 1, StoryRecording::Active)},
                                                                    {30, recording(30, 1, StoryRecording::Closing)},
                                                                    {40, recording(40, 1, StoryRecording::Unresolved)}};
    EXPECT_EQ(ids(storiesToStartOnRegistration(active, 1)), (std::vector<StoryId>{20}));
}

// The stop goes to the first acquire's starts that succeeded, as before, and
// to every start sent to a keeper that registered later, whatever its answer:
// a failed call may still have run on that keeper.
TEST(KeeperRestartStories, StopReachesEveryLaterStartAndTheSucceededFirstOnes)
{
    StoryRecording r;
    r.starts = {settled(chronolog::CL_SUCCESS), settled(chronolog::CL_ERR_UNKNOWN), settled(chronolog::CL_SUCCESS)};
    r.originalStarts = r.starts.size();
    r.starts.push_back(settled(chronolog::CL_ERR_UNKNOWN, false)); // timed out on the registering keeper
    r.starts.push_back(settled(chronolog::CL_SUCCESS));

    auto stop = storyStopTargets(r);
    EXPECT_EQ(stop.state, StoryStopTargets::Ready);
    EXPECT_EQ(stop.targets, (std::vector<std::size_t>{0, 2, 3, 4}));
}

// A start still in flight to a registering keeper holds the stop back: sent
// now, the stop could reach the keeper before the start and leave it a
// pipeline that no stop ever reaches.
TEST(KeeperRestartStories, StopWaitsForAStartInFlight)
{
    StoryRecording r;
    r.starts = {settled(chronolog::CL_SUCCESS), settled(chronolog::CL_SUCCESS)};
    r.originalStarts = r.starts.size();
    r.starts.push_back(std::make_shared<AdminCompletion>());

    EXPECT_EQ(storyStopTargets(r).state, StoryStopTargets::Pending);
    r.starts.back()->status = chronolog::CL_SUCCESS;
    r.starts.back()->confirmed = true;
    r.starts.back()->done.store(true);
    EXPECT_EQ(storyStopTargets(r).state, StoryStopTargets::Ready);
}

// A start of the first acquire with no confirmed answer stays unresolved, as
// it did before keepers could register into an active story.
TEST(KeeperRestartStories, UnconfirmedFirstStartStaysUnresolved)
{
    StoryRecording r;
    r.starts = {settled(chronolog::CL_SUCCESS), settled(chronolog::CL_ERR_OUTCOME_UNCONFIRMED, false)};
    r.originalStarts = r.starts.size();
    EXPECT_EQ(storyStopTargets(r).state, StoryStopTargets::Unconfirmed);
}
