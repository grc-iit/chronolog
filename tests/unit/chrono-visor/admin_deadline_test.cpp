#include "admin_test_fixture.h"
#include <private/LifecycleStep.h>
#include <algorithm>
TEST_P(AdminDeadline, TwoSilentKeepersDoNotHideResponsiveThird)
{
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 123, roster, player),
              CL_SUCCESS);
    auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, 220ms);
    ASSERT_EQ(roster.size(), 1u);
    EXPECT_EQ(to_string(roster.front()), to_string(keeper(3)));
    EXPECT_EQ(to_string(player), to_string(service(5)));
    {
        std::lock_guard<std::mutex> lock(rosterMutex);
        EXPECT_EQ(playerRoster.size(), 1u);
    }
    EXPECT_EQ(starts[1].load(), 1);
    EXPECT_EQ(starts[2].load(), 1);
    EXPECT_EQ(starts[3].load(), 1);
}
// Keepers 1 and 2 answer the start only after its attempt timed out, so the
// acquire gets keeper 3 alone and their starts stay without an answer. The
// release still stops the story, on every component, and the next acquire
// starts it afresh on all three keepers. Before, every release of such a story
// returned CL_ERR_OUTCOME_UNCONFIRMED until the visor restarted, and an
// acquire kept reusing the one-keeper roster.
// The registry uses the size of the runtime it is given (rpc_slots), and an
// acquire of a new story needs its own request, the grapher, the player and
// every keeper of the group at once.
TEST(VisorRpcSlots, RegistryUsesTheRuntimesSizeAndAStartNeedsKeepersPlusThree)
{
    // The registry holds the runtime weakly; the visor's VisorData owns it.
    auto runtime = std::make_shared<rpc::RpcRuntime>(7);
    auto registry = KeeperRegistry::CreateOwned(RpcTimeoutPolicy{}, runtime);
    EXPECT_EQ(registry->adminOwner()->runtime.capacity(), 7u);
    EXPECT_EQ(chronolog::storyStartSlots(2), 5u);
    EXPECT_EQ(chronolog::storyStartSlots(29), 32u);
}
// A refusal is reported at once, then at most once a second with the number
// refused since the last report.
TEST(VisorRpcSlots, RefusalLogReportsAtMostOnceASecondWithTheCount)
{
    chronolog::RefusalLog log;
    EXPECT_EQ(log.note(), 1u);
    EXPECT_EQ(log.note(), 0u);
    EXPECT_EQ(log.note(), 0u);
    std::this_thread::sleep_for(1050ms);
    EXPECT_EQ(log.note(), 3u);
    EXPECT_EQ(log.note(), 0u);
}
// The grapher answers a destroy after the visor's 120 ms deadline, so the
// destroy's outcome is unknown. Before, the visor kept a marker for it that
// nothing cleared: every later destroy, create or acquire of the name returned
// -23 until the visor restarted.
TEST_P(AdminDeadline, UnconfirmedStoryDestroyIsSentAgainByTheNextDestroy)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 2);
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_ERR_NOT_EXIST);
}
TEST_P(AdminDeadline, UnconfirmedStoryDestroyIsFinishedByTheNextAcquire)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    auto again = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(again.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(again.getKeepers().size(), 3u);
    EXPECT_EQ(storyDestroys[0].load(), 2);
    EXPECT_EQ(starts[0].load(), 2);
}
// The CI case: a chronicle destroy that one grapher did not confirm in time,
// then another client creates the chronicle again and acquires a story in it.
TEST_P(AdminDeadline, UnconfirmedChronicleDestroyIsFinishedByTheNextCreate)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyChronicle(100, "chronicle"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    EXPECT_EQ(portal.CreateChronicle(100, "chronicle"), CL_SUCCESS);
    EXPECT_EQ(chronicleDestroys[0].load(), 2);
    auto acquired = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(acquired.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(acquired.getKeepers().size(), 3u);
}
// An acquire finishes the chronicle's destroy too, and then finds no chronicle.
TEST_P(AdminDeadline, AcquireFinishesAnUnconfirmedChronicleDestroyAndFindsNoChronicle)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyChronicle(100, "chronicle"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_NOT_EXIST);
    EXPECT_EQ(chronicleDestroys[0].load(), 2);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    EXPECT_EQ(portal.CreateChronicle(100, "chronicle"), CL_SUCCESS);
}
TEST_P(AdminDeadline, DegradedAcquireIsReleasedAndTheNextAcquireReachesEveryKeeper)
{
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 940, roster, player),
              CL_SUCCESS);
    ASSERT_EQ(roster.size(), 1u);
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStop(940), CL_SUCCESS);
    for(int i = 0; i < 5; ++i) EXPECT_EQ(stops[i].load(), 1) << "component " << i;
    // Let the late start handlers finish before the next acquire.
    std::this_thread::sleep_for(400ms);
    delays[1] = delays[2] = 0;
    roster.clear();
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 940, roster, player),
              CL_SUCCESS);
    EXPECT_EQ(roster.size(), 3u);
}
// Keeper 2 confirms the start 20 ms after keeper 3: the acquire waits for it
// instead of fixing the roster on the first keeper's answer. Keeper 1 stays
// silent and is left out once the wait ends.
TEST_P(AdminDeadline, KeeperConfirmingShortlyAfterTheFirstJoinsTheRoster)
{
    delays[2] = 20;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 126, roster, player),
              CL_SUCCESS);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    std::vector<std::string> names;
    for(auto const& k: roster) names.push_back(to_string(k));
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected{to_string(keeper(2)), to_string(keeper(3))};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(names, expected);
}
TEST_P(AdminDeadline, NoResponsiveKeeperFailsWithoutBlindCompensatingStop)
{
    delays[3] = 300;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    auto begin = std::chrono::steady_clock::now();
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 124, roster, player),
              CL_SUCCESS);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    EXPECT_TRUE(roster.empty());
    EXPECT_EQ(stops[0].load() + stops[1].load() + stops[2].load() + stops[3].load(), 0);
}
TEST_P(AdminDeadline, LateSetupCannotBecomeConfirmedRosterOnAnotherAcquire)
{
    delays[3] = 300;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 125, roster, player),
              CL_SUCCESS);
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("chronicle", "story", 125, roster, player),
              CL_SUCCESS);
    EXPECT_TRUE(roster.empty());
}

TEST_P(AdminDeadline, ThreeSlotsReachThirdNativePeerWhileFirstTwoAreSilent)
{
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(120ms);
    std::vector<rpc::RpcReservation> tokens;
    for(int i = 0; i < 3; ++i)
    {
        auto token = owner->runtime.reserve(deadline);
        ASSERT_TRUE(token);
        tokens.push_back(std::move(token));
    }
    std::vector<rpc::RpcPending<int>> pending;
    std::vector<std::shared_ptr<AdminCompletion>> completions;
    for(int i = 0; i < 3; ++i)
    {
        auto c = std::make_shared<AdminCompletion>();
        completions.push_back(c);
        std::string address;
        service(i + 2).get_service_as_string(address);
        auto descriptor = DataStoreAdminClient::CreateDataStoreAdminClient(owner, address, i + 2);
        pending.push_back(owner->runtime.submit<int>(
                std::move(tokens[i]),
                std::make_shared<AdminWork>(descriptor,
                                            AdminArguments{AdminAction::Start, "c", "s", 900, 0, {}},
                                            c,
                                            registry)));
    }
    auto a = pending[0].wait();
    auto b = pending[1].wait();
    auto c = pending[2].wait();
    EXPECT_FALSE(a.reply);
    EXPECT_FALSE(b.reply);
    ASSERT_TRUE(c.reply);
    EXPECT_EQ(*c.reply, CL_SUCCESS);
    EXPECT_EQ(starts[3].load(), 1);
}
// #743: admin clients on one engine share the RPC IDs of the names they call.
// On develop before 6c364840 a client's destructor deregistered them, so once
// the visor deleted the admin client of a keeper that left, it could not
// start a story on the keepers that stayed and every acquire failed with
// CL_ERR_NO_KEEPERS. Here the clients hold no registration (the admin runtime
// does); these tests keep it that way.
static bool destroyedSoon(std::weak_ptr<DataStoreAdminClient> const& client)
{
    auto until = std::chrono::steady_clock::now() + 3s;
    while(!client.expired() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    return client.expired();
}
TEST_P(AdminDeadline, DeletingOneAdminClientLeavesTheOtherWorking)
{
    delays[1] = delays[2] = 0;
    auto owner = registry->adminOwner();
    auto clientFor = [&](int provider)
    {
        std::string address;
        service(provider).get_service_as_string(address);
        return DataStoreAdminClient::CreateDataStoreAdminClient(owner, address, static_cast<uint16_t>(provider));
    };
    auto call = [&](std::shared_ptr<DataStoreAdminClient> client, AdminAction action)
    {
        auto token = owner->runtime.reserve(rpc::RpcDeadline::after(1s));
        EXPECT_TRUE(token);
        return owner->runtime
                .submit<int>(std::move(token),
                             std::make_shared<AdminWork>(std::move(client),
                                                         AdminArguments{action, "c", "s", 910, 0, {}},
                                                         std::make_shared<AdminCompletion>(),
                                                         registry))
                .wait();
    };
    auto leaving = clientFor(2);
    auto staying = clientFor(3);
    auto first = call(leaving, AdminAction::Start);
    ASSERT_TRUE(first.reply);
    ASSERT_EQ(*first.reply, CL_SUCCESS);
    std::weak_ptr<DataStoreAdminClient> gone = leaving;
    leaving.reset();
    ASSERT_TRUE(destroyedSoon(gone));
    auto start = call(staying, AdminAction::Start);
    auto stop = call(staying, AdminAction::Stop);
    ASSERT_TRUE(start.reply);
    EXPECT_EQ(*start.reply, CL_SUCCESS);
    ASSERT_TRUE(stop.reply);
    EXPECT_EQ(*stop.reply, CL_SUCCESS);
    EXPECT_EQ(starts[1].load(), 1);
    EXPECT_EQ(starts[2].load(), 1);
    EXPECT_EQ(stops[2].load(), 1);
}
// #743 as the visor meets it, with a keeper that comes back: it unregisters,
// registers again at the same address, and the registry replaces its admin
// client and deletes the old one. The next acquire still starts the story on
// every keeper, the returning one and the two that stayed.
TEST_P(AdminDeadline, AdminClientReplacedForAReturningKeeperLeavesEveryKeeperWorking)
{
    delays[1] = delays[2] = 0;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "first", 920, roster, player), CL_SUCCESS);
    ASSERT_EQ(roster.size(), 3u);
    // A recording keeps its components' admin clients for its stop; stop it
    // so that only the registry holds keeper 1's.
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStop(920), CL_SUCCESS);
    auto old = KeeperRegistryTestAccess::keeperAdminClient(*registry, keeper(1));
    ASSERT_FALSE(old.expired());
    ASSERT_EQ(registry->unregisterKeeperProcess(keeper(1)), CL_SUCCESS);
    // The old client is deleted on registration once its delayed-exit time
    // (0 s here, at one-second resolution) has passed.
    std::this_thread::sleep_for(1100ms);
    ASSERT_EQ(registry->registerKeeperProcess(KeeperRegistrationMsg(keeper(1), service(2))), CL_SUCCESS);
    EXPECT_TRUE(destroyedSoon(old));
    roster.clear();
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "second", 921, roster, player), CL_SUCCESS);
    EXPECT_EQ(roster.size(), 3u);
    EXPECT_EQ(starts[1].load(), 2);
    EXPECT_EQ(starts[2].load(), 2);
    EXPECT_EQ(starts[3].load(), 2);
}
// A keeper that leaves for good: once its delayed exit has passed (0 s in this
// fixture), the next acquire frees its admin client and registry entry, as
// develop's getActiveKeepers() does, and starts the story on the others.
TEST_P(AdminDeadline, AcquireAfterAKeeperLeftFreesItsAdminClient)
{
    delays[1] = delays[2] = 0;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "first", 930, roster, player), CL_SUCCESS);
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStop(930), CL_SUCCESS);
    auto gone = KeeperRegistryTestAccess::keeperAdminClient(*registry, keeper(1));
    ASSERT_FALSE(gone.expired());
    ASSERT_EQ(KeeperRegistryTestAccess::keeperEntries(*registry), 3u);
    ASSERT_EQ(registry->unregisterKeeperProcess(keeper(1)), CL_SUCCESS);
    std::this_thread::sleep_for(1100ms);
    roster.clear();
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "second", 931, roster, player), CL_SUCCESS);
    EXPECT_TRUE(destroyedSoon(gone));
    EXPECT_EQ(KeeperRegistryTestAccess::keeperEntries(*registry), 2u);
    std::vector<std::string> keepers;
    for(auto const& k: roster) keepers.push_back(to_string(k));
    std::sort(keepers.begin(), keepers.end());
    std::vector<std::string> expected{to_string(keeper(2)), to_string(keeper(3))};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(keepers, expected);
}
TEST_P(AdminDeadline, OccupiedRosterFailsBeforeAnySetupDispatch)
{
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(120ms);
    std::vector<rpc::RpcReservation> occupied;
    for(int i = 0; i < 4; ++i)
    {
        auto t = owner->runtime.reserve(deadline);
        ASSERT_TRUE(t);
        occupied.push_back(std::move(t));
    }
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 500, roster, player), CL_ERR_RPC_CAPACITY);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
}
TEST_P(AdminDeadline, ExplicitKeeperRejectionDoesNotEnterConfirmedRoster)
{
    delays[1] = delays[2] = 0;
    replies[1] = CL_ERR_UNKNOWN;
    replies[2] = CL_ERR_UNKNOWN;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 501, roster, player), CL_SUCCESS);
    ASSERT_EQ(roster.size(), 1u);
    EXPECT_EQ(to_string(roster[0]), to_string(keeper(3)));
}
TEST_P(AdminDeadline, PlayerSetupUsesOriginalBudgetWithoutCompensatingStop)
{
    delays[0] = 70;
    delays[1] = delays[2] = 0;
    delays[4] = 100;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    auto begin = std::chrono::steady_clock::now();
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 502, roster, player), CL_SUCCESS);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    EXPECT_TRUE(roster.empty());
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
}
TEST_P(AdminDeadline, UnregisterAndReplaceDescriptorWhileOldSetupCompletes)
{
    delays[3] = 300;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 503, roster, player), CL_SUCCESS);
    EXPECT_EQ(registry->unregisterKeeperProcess(keeper(1)), CL_SUCCESS);
    EXPECT_EQ(registry->registerKeeperProcess(KeeperRegistrationMsg(keeper(1), service(2))), CL_SUCCESS);
    std::this_thread::sleep_for(330ms);
    EXPECT_NE(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 503, roster, player), CL_SUCCESS);
    EXPECT_TRUE(roster.empty());
    // The second acquire first settles the first one: the grapher confirmed
    // its start and the keepers' starts timed out, so each gets one stop; the
    // player's start was never sent and gets none. Its own fresh start then
    // fails as well, since the keepers are still slow.
    for(int i = 0; i < 4; ++i) EXPECT_EQ(stops[i].load(), 1) << "component " << i;
    EXPECT_EQ(stops[4].load(), 0);
}
// A keeper that registers again while clients hold more stories than the
// admin runtime has free slots (12 against the fixture's 8) gets every story
// started: the first 8 at once, the rest as their slots come free.
TEST_P(AdminDeadline, HeldStoriesBeyondTheFreeSlotsAreStartedInWaves)
{
    delays[1] = delays[2] = 0;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    for(int i = 0; i < 12; ++i)
    {
        int status = CL_ERR_RPC_CAPACITY;
        auto until = std::chrono::steady_clock::now() + 1s;
        while(status == CL_ERR_RPC_CAPACITY && std::chrono::steady_clock::now() < until)
        {
            status = registry->notifyRecordingGroupOfStoryRecordingStart("chronicle",
                                                                         "story" + std::to_string(i),
                                                                         600 + i,
                                                                         roster,
                                                                         player);
            if(status == CL_ERR_RPC_CAPACITY)
                std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(status, CL_SUCCESS) << "story " << i;
    }
    int const before = starts[1].load();
    EXPECT_EQ(before, 12);
    // Let the acquires' work give its slots back, so all 8 are free.
    std::this_thread::sleep_for(100ms);
    delays[1] = 50; // each start holds its slot for 50 ms
    ASSERT_EQ(registry->unregisterKeeperProcess(keeper(1)), CL_SUCCESS);
    ASSERT_EQ(registry->registerKeeperProcess(KeeperRegistrationMsg(keeper(1), service(2))), CL_SUCCESS);
    auto until = std::chrono::steady_clock::now() + 2s;
    while(starts[1].load() < before + 12 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(starts[1].load(), before + 12) << "held stories left without a start on the registering keeper";
}
TEST_P(AdminDeadline, LateStopBlocksAnotherAcquirerAndRecreation)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    ClientId second = 200;
    uint64_t offset = 0;
    ASSERT_EQ(portal.ClientConnect(0, second, offset), CL_SUCCESS);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    stopDelays[1] = 300;
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_OUTCOME_UNCONFIRMED);
    auto response = portal.AcquireStory(200, "chronicle", "story");
    EXPECT_NE(response.getErrorCode(), CL_SUCCESS);
    EXPECT_TRUE(response.getKeepers().empty());
    EXPECT_NE(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(starts[0].load(), 1);
}
// A stop the keeper did not confirm leaves the story closing, and the next
// acquire sends that stop again. Once the keeper answers, the acquire succeeds
// with a new recording instead of being refused until the visor restarts (a
// keeper or grapher that was only paused when the story was released).
TEST_P(AdminDeadline, UnconfirmedStopIsSentAgainByTheNextAcquire)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal);
    ClientId second = 200;
    uint64_t offset = 0;
    ASSERT_EQ(portal.ClientConnect(0, second, offset), CL_SUCCESS);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    stopDelays[1] = 300;
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_OUTCOME_UNCONFIRMED);
    int const stopsAfterRelease = stops[1].load();
    EXPECT_EQ(stopsAfterRelease, 1);
    stopDelays[1] = 0; // the keeper answers again
    // Let the first stop's 300 ms handler finish, so its work no longer holds
    // admin slots; while it does, the acquire is refused as busy.
    std::this_thread::sleep_for(400ms);
    auto response = portal.AcquireStory(200, "chronicle", "story");
    EXPECT_EQ(response.getErrorCode(), CL_SUCCESS);
    EXPECT_FALSE(response.getKeepers().empty());
    EXPECT_GT(stops[1].load(), stopsAfterRelease) << "the stop was not sent again";
    EXPECT_EQ(starts[0].load(), 2) << "no new recording was started";
    EXPECT_EQ(portal.ReleaseStory(200, "chronicle", "story"), CL_SUCCESS);
}
TEST_P(AdminDeadline, UnresolvedStartIsSettledByAnotherClientsAcquire)
{
    // The player answers its start only after the acquire's budget, so the
    // first acquire fails and leaves the story Unresolved; its client keeps
    // the acquisition. Another client's acquire stops every component whose
    // start may have run, then starts the story anew, instead of refusing it
    // until the visor restarts.
    delays[1] = delays[2] = 0;
    delays[4] = 300;
    VisorClientPortal portal;
    attach(portal);
    ClientId second = 200;
    uint64_t offset = 0;
    ASSERT_EQ(portal.ClientConnect(0, second, offset), CL_SUCCESS);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_OUTCOME_UNCONFIRMED);
    // Let the player's 300 ms handler finish, so its work holds no slot.
    std::this_thread::sleep_for(400ms);
    delays[4] = 0;
    auto response = portal.AcquireStory(200, "chronicle", "story");
    EXPECT_EQ(response.getErrorCode(), CL_SUCCESS);
    EXPECT_FALSE(response.getKeepers().empty());
    EXPECT_EQ(stops[0].load(), 1) << "the grapher's start was not stopped";
    EXPECT_EQ(stops[4].load(), 1) << "the player's unconfirmed start was not stopped";
    EXPECT_EQ(starts[0].load(), 2) << "no new recording was started";
    EXPECT_EQ(portal.ReleaseStory(200, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
}
TEST_P(AdminDeadline, UnresolvedStartIsStoppedByItsHoldersRelease)
{
    // As above, but the client whose acquire failed releases the story: the
    // release stops every component whose start may have run, and the same
    // client can then acquire the story again.
    delays[1] = delays[2] = 0;
    delays[4] = 300;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_OUTCOME_UNCONFIRMED);
    std::this_thread::sleep_for(400ms);
    delays[4] = 0;
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(stops[0].load(), 1) << "the grapher's start was not stopped";
    EXPECT_EQ(stops[4].load(), 1) << "the player's unconfirmed start was not stopped";
    auto response = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(response.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(starts[0].load(), 2) << "no new recording was started";
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
}
TEST_P(AdminDeadline, SaturationDefault32StartsNoMutationOrFanout)
{
    VisorClientPortal portal;
    attach(portal);
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(120ms);
    auto tokens = occupy(32, deadline);
    ASSERT_EQ(tokens.size(), 32u);
    auto extra = owner->runtime.reserve(deadline);
    EXPECT_FALSE(extra);
    EXPECT_EQ(extra.status(), CL_ERR_RPC_CAPACITY);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_RPC_CAPACITY);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
}
TEST_P(AdminDeadline, PortalMutexWaitConsumesOriginalBudgetWithoutMetadataMutation)
{
    VisorClientPortal portal;
    attach(portal);
    std::unique_lock<std::mutex> held(portal.core()->operationMutex);
    auto begin = std::chrono::steady_clock::now();
    auto failed = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_NE(failed.getErrorCode(), CL_SUCCESS);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    held.unlock();
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
}
TEST_P(AdminDeadline, RegistryMutexWaitConsumesOriginalBudgetBeforeDispatch)
{
    auto held = KeeperRegistryTestAccess::lock(*registry);
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 504, roster, player),
              CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    held.unlock();
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 504, roster, player), CL_SUCCESS);
}

TEST_P(AdminDeadline, LocalRosterCapacityRejectionRollsBackOnlyItsOwnAcquisition)
{
    VisorClientPortal portal;
    attach(portal);
    auto tokens = occupy(4, rpc::RpcDeadline::after(120ms));
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_RPC_CAPACITY);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
    tokens.clear();
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
}
// The failed acquire sends no stop and keeps client 100's acquisition: the
// same client's next acquire settles the recording and gets the story, and one
// release by it is the last.
TEST_P(AdminDeadline, RemoteCapacityErrorAfterPartialSetupRetainsAcquisition)
{
    delays[1] = delays[2] = 0;
    replies[4] = CL_ERR_RPC_CAPACITY;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_RPC_CAPACITY);
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
    replies[4] = CL_SUCCESS;
    auto again = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(again.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(again.getKeepers().size(), 3u);
    auto stopsBefore = stops[0].load();
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(stops[0].load(), stopsBefore + 1);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_ACQUIRED);
}
// The failed acquire sends no stop and keeps client 100's acquisition: the
// same client's next acquire settles the recording and gets the story, and one
// release by it is the last.
TEST_P(AdminDeadline, RemoteDeadlineErrorAfterPartialSetupRetainsAcquisition)
{
    delays[1] = delays[2] = 0;
    replies[4] = CL_ERR_DEADLINE_EXCEEDED;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_DEADLINE_EXCEEDED);
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
    replies[4] = CL_SUCCESS;
    auto again = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(again.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(again.getKeepers().size(), 3u);
    auto stopsBefore = stops[0].load();
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(stops[0].load(), stopsBefore + 1);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_ACQUIRED);
}
// Every keeper rejects its start after the grapher's ran, so the acquire fails
// but keeps client 100's acquisition. It is reported as unknown, not as
// CL_ERR_NO_KEEPERS, which a client would read as "not acquired". Once the
// keepers accept, the same client's next acquire gets the story.
TEST_P(AdminDeadline, AcquisitionKeptAfterEveryKeeperRejectedIsReportedUnknown)
{
    delays[1] = delays[2] = 0;
    replies[1] = replies[2] = replies[3] = CL_ERR_UNKNOWN;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_OUTCOME_UNCONFIRMED);
    replies[1] = replies[2] = replies[3] = CL_SUCCESS;
    auto again = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(again.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(again.getKeepers().size(), 3u);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
}
TEST_P(AdminDeadline, ConfirmedRejectedKeeperSetupNeverReceivesStop)
{
    delays[1] = delays[2] = 0;
    replies[1] = replies[2] = CL_ERR_UNKNOWN;
    std::vector<KeeperIdCard> roster;
    ServiceId player;
    ASSERT_EQ(registry->notifyRecordingGroupOfStoryRecordingStart("c", "s", 505, roster, player), CL_SUCCESS);
    EXPECT_EQ(registry->notifyRecordingGroupOfStoryRecordingStop(505), CL_SUCCESS);
    EXPECT_EQ(stops[1].load(), 0);
    EXPECT_EQ(stops[2].load(), 0);
    EXPECT_EQ(stops[0].load(), 1);
    EXPECT_EQ(stops[3].load(), 1);
    EXPECT_EQ(stops[4].load(), 1);
}

namespace
{
struct ShutdownProbe
{
    std::weak_ptr<KeeperRegistry> registry;
    std::weak_ptr<void> provider;
    std::weak_ptr<tl::engine> engine;
    std::atomic<unsigned> finalizers{0};
};
class ShutdownLookupGate final: public AdminLookup
{
public:
    explicit ShutdownLookupGate(std::shared_ptr<ShutdownProbe> p)
        : probe(std::move(p))
    {}
    std::atomic<bool> entered{false}, released{false};
    std::shared_ptr<ShutdownProbe> probe;
    tl::endpoint resolve(tl::engine& engine, std::string const& address) override
    {
        entered.store(true, std::memory_order_release);
        while(!released.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        if(probe->finalizers.load() != 0)
            throw std::runtime_error("RED engine finalized during lookup gate");
        return engine.lookup(address);
    }
};
template <class Predicate>
bool eventually(Predicate predicate)
{
    auto until = std::chrono::steady_clock::now() + 3s;
    while(!predicate() && std::chrono::steady_clock::now() < until)
    {
        ABT_thread_yield();
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
} // namespace
class RegistryShutdownOwnership: public AdminDeadline
{
protected:
    std::shared_ptr<ShutdownProbe> probe = std::make_shared<ShutdownProbe>();
    std::shared_ptr<ShutdownLookupGate> gate;
    std::shared_ptr<AdminObservation> observation;
    std::shared_ptr<AdminLookup> lookupDependency() override { return gate; }
    void SetUp() override
    {
        auto name = std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        if(name.find("Lookup") != std::string::npos)
            gate = std::make_shared<ShutdownLookupGate>(probe);
        AdminDeadline::SetUp();
        probe->registry = registry;
        probe->provider = KeeperRegistryTestAccess::provider(*registry);
        probe->engine = registry->adminOwner()->engine;
        observation = registry->adminOwner()->observation;
        registry->adminOwner()->engine->push_finalize_callback(probe.get(), [p = probe]() { ++p->finalizers; });
    }
    void checkIsolatedInvalidShutdownAck(int ack)
    {
        for(int i = 1; i <= 3; ++i) ASSERT_EQ(registry->unregisterKeeperProcess(keeper(i)), CL_SUCCESS);
        ASSERT_EQ(registry->unregisterPlayerProcess(PlayerIdCard(1, service(5))), CL_SUCCESS);
        shutdownAcks[0] = ack;
        EXPECT_EQ(registry->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
        EXPECT_EQ(registry->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
        EXPECT_EQ(shutdownEntries[0].load(), 1u);
        for(int i = 1; i < 5; ++i) EXPECT_EQ(shutdownEntries[i].load(), 0u);
        registry.reset();
        EXPECT_TRUE(eventually(
                [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
        EXPECT_EQ(probe->finalizers.load(), 1u);
    }
};
TEST_P(RegistryShutdownOwnership, ShutdownLookupRetainsActualOwnerUntilNativeWorkSettles)
{
    std::atomic<int> result{CL_ERR_UNKNOWN};
    std::atomic<bool> returned{false};
    std::thread close(
            [lease = registry, &result, &returned]()
            {
                result = lease->ShutdownRegistryService();
                returned.store(true, std::memory_order_release);
            });
    EXPECT_TRUE(eventually([&]() { return gate->entered.load(std::memory_order_acquire); }));
    registry.reset();
    EXPECT_TRUE(eventually([&]() { return returned.load(std::memory_order_acquire); }));
    close.join();
    EXPECT_EQ(result.load(), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_FALSE(probe->registry.expired());
    EXPECT_FALSE(probe->provider.expired());
    EXPECT_FALSE(probe->engine.expired());
    EXPECT_EQ(probe->finalizers.load(), 0u);
    if(auto lease = probe->registry.lock())
    {
        EXPECT_EQ(lease->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
    }
    gate->released.store(true, std::memory_order_release);
    EXPECT_TRUE(eventually(
            [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
    EXPECT_EQ(probe->finalizers.load(), 1u);
    for(auto const& count: shutdownEntries) EXPECT_EQ(count.load(), 0u);
}
TEST_P(RegistryShutdownOwnership, ShutdownMissingAckRetainsTheUntimedNativeContinuation)
{
    suppressShutdownReply[0] = true;
    for(int i = 1; i <= 3; ++i) ASSERT_EQ(registry->unregisterKeeperProcess(keeper(i)), CL_SUCCESS);
    std::atomic<int> result{CL_ERR_UNKNOWN};
    std::atomic<bool> returned{false};
    std::thread close(
            [lease = registry, &result, &returned]()
            {
                result = lease->ShutdownRegistryService();
                returned.store(true, std::memory_order_release);
            });
    EXPECT_TRUE(eventually([&]() { return shutdownEntries[0].load() != 0; }));
    EXPECT_TRUE(eventually([&]() { return returned.load(std::memory_order_acquire); }));
    close.join();
    // Closing only the mock peer creates a real native no-reply transport fault.
    // It is not a proof of remote cancellation or registry-side settlement.
    peers->finalize();
    peerFinalized = true;
    // A missing reply can leave an untimed call running even after the peer
    // engine exits. Neither worker return nor peer death proves settlement.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(probe->finalizers.load(), 0u);
    EXPECT_EQ(shutdownEntries[0].load(), 1u);
    registry.reset();
    EXPECT_EQ(result.load(), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_FALSE(probe->registry.expired());
    EXPECT_FALSE(probe->provider.expired());
    EXPECT_FALSE(probe->engine.expired());
    EXPECT_EQ(probe->finalizers.load(), 0u);
    if(auto lease = probe->registry.lock())
    {
        EXPECT_EQ(lease->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
    }
    EXPECT_FALSE(probe->registry.expired());
    EXPECT_EQ(probe->finalizers.load(), 0u);
}
TEST_P(RegistryShutdownOwnership, ShutdownCollectionRetainsLateAckWithoutDispatchingAnotherJoin)
{
    holdShutdown[0] = true;
    struct Release
    {
        std::atomic<bool>& held;
        ~Release() { held = false; }
    } release{holdShutdown[0]};
    auto owner = registry->adminOwner();
    std::string address;
    service(1).get_service_as_string(address);
    auto client = DataStoreAdminClient::CreateDataStoreAdminClient(owner, address, 1);
    auto began = rpc::RpcDeadline::Clock::now();
    EXPECT_NE(CL_SUCCESS, client->shutdown_collection(registry, rpc::RpcDeadline::fromStart(began, 80ms)));
    EXPECT_LT(rpc::RpcDeadline::Clock::now() - began, 250ms);
    ASSERT_TRUE(eventually([&] { return shutdownEntries[0].load() == 1; }));
    // Exceed the fixture's native attempt cap while the original live join is held.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(0u, observation->shutdownNoReplyRunReturns.load());
    holdShutdown[0] = false;
    EXPECT_EQ(CL_SUCCESS, client->shutdown_collection(registry, rpc::RpcDeadline::after(1s)));
    EXPECT_EQ(1u, shutdownEntries[0].load());
    EXPECT_EQ(0u, observation->shutdownNoReplyRunReturns.load());
}
TEST_P(RegistryShutdownOwnership, ShutdownRetryKeepsDependenciesUntilKeeperUnregisters)
{
    holdShutdown[1] = true;
    struct Release
    {
        std::atomic<bool>& held;
        ~Release() { held = false; }
    } release{holdShutdown[1]};
    EXPECT_EQ(registry->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(shutdownEntries[0].load(), 0u); // Grapher still receives Keeper chunks.
    EXPECT_EQ(shutdownEntries[4].load(), 0u);
    holdShutdown[1] = false;
    // These empty mock Keepers have no pending data. Only their matching
    // unregister permits dependency collection to stop; ACK1 alone cannot.
    for(int i = 1; i <= 3; ++i) ASSERT_EQ(registry->unregisterKeeperProcess(keeper(i)), CL_SUCCESS);
    EXPECT_TRUE(eventually([&] { return registry->ShutdownRegistryService() == CL_SUCCESS; }));
    for(int i = 0; i <= 4; ++i) EXPECT_EQ(shutdownEntries[i].load(), 1u);
    registry.reset();
    EXPECT_TRUE(eventually(
            [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
    EXPECT_EQ(probe->finalizers.load(), 1u);
    // This is local registry/collection phase cleanup, never daemon retirement.
}

TEST_P(RegistryShutdownOwnership, LocalDropStartsNoImplicitRemoteShutdown)
{
    registry.reset();
    EXPECT_TRUE(eventually(
            [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
    EXPECT_EQ(probe->finalizers.load(), 1u);
    for(auto const& count: shutdownEntries) EXPECT_EQ(count.load(), 0u);
}
TEST_P(RegistryShutdownOwnership, ShutdownFalseAckIsIncomplete) { checkIsolatedInvalidShutdownAck(0); }
TEST_P(RegistryShutdownOwnership, ShutdownUnexpectedPositiveAckIsIncomplete) { checkIsolatedInvalidShutdownAck(2); }
TEST_P(RegistryShutdownOwnership, ShutdownNegativeAckIsIncomplete) { checkIsolatedInvalidShutdownAck(CL_ERR_UNKNOWN); }
INSTANTIATE_TEST_SUITE_P(Native, RegistryShutdownOwnership, ::testing::Values("ofi+sockets", "ofi+tcp"));
INSTANTIATE_TEST_SUITE_P(Native, AdminDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));

TEST(VisorOwnedRuntime, PortalAndRegistryConsumeTheOriginal32Slots)
{
    auto actual = std::make_shared<rpc::RpcRuntime>(32);
    auto registry = KeeperRegistry::CreateOwned({}, actual);
    VisorClientPortal portal;
    portal.core()->registry = registry;
    auto deadline = rpc::RpcDeadline::after(1s);
    std::vector<rpc::RpcReservation> held;
    for(unsigned n = 0; n != 32; ++n)
    {
        auto slot = actual->reserve(deadline);
        ASSERT_TRUE(slot);
        held.push_back(std::move(slot));
    }
    EXPECT_EQ(registry->adminOwner()->runtime.reserve(deadline).status(), CL_ERR_RPC_CAPACITY);
    EXPECT_EQ(portal.core()->Invoke(VisorControlAction::Connect, 123)->status, CL_ERR_RPC_CAPACITY);
    held.clear();
    EXPECT_EQ(actual->drain(rpc::RpcDeadline::after(1s)), CL_SUCCESS);
}
TEST(VisorOwnedRuntime, ReservedWorkStillDiesOnItsOriginalWorkerAfterPublicHandleDrop)
{
    auto actual = std::make_shared<rpc::RpcRuntime>(32);
    std::weak_ptr<rpc::RpcRuntime> weak = actual;
    auto registry = KeeperRegistry::CreateOwned({}, actual);
    auto token = actual->reserve(rpc::RpcDeadline::after(1s));
    ASSERT_TRUE(token);
    actual.reset();
    ASSERT_TRUE(weak.expired()); // No idle registry-to-runtime backlink.
    struct Observed final: rpc::OwnedRpcWork<int>
    {
        std::thread::id caller;
        std::atomic<bool>& destroyed;
        std::atomic<bool>& onCaller;
        Observed(std::thread::id caller, std::atomic<bool>& destroyed, std::atomic<bool>& onCaller)
            : caller(caller)
            , destroyed(destroyed)
            , onCaller(onCaller)
        {}
        ~Observed() override
        {
            onCaller.store(std::this_thread::get_id() == caller, std::memory_order_relaxed);
            destroyed.store(true, std::memory_order_release);
        }
        rpc::RpcResult<int> run(rpc::OperationContext&) override { return {CL_SUCCESS, false, 0}; }
    };
    std::atomic<bool> destroyed{false}, onCaller{false};
    registry->adminOwner()
            ->runtime
            .submit<int>(std::move(token), std::make_shared<Observed>(std::this_thread::get_id(), destroyed, onCaller))
            .wait();
    auto expiry = std::chrono::steady_clock::now() + 1s;
    while(!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < expiry)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(destroyed.load(std::memory_order_acquire));
    EXPECT_FALSE(onCaller.load());
}

TEST(VisorOwnedRuntime, ExpiredReservationUsesOriginalDeadlineAndWorkerAfterHandleDrop)
{
    auto actual = std::make_shared<rpc::RpcRuntime>(32);
    auto registry = KeeperRegistry::CreateOwned({}, actual);
    auto token = actual->reserve(rpc::RpcDeadline::after(10ms));
    ASSERT_TRUE(token);
    actual.reset();
    std::this_thread::sleep_for(20ms);
    struct Expired final: rpc::OwnedRpcWork<int>
    {
        std::atomic<bool>& destroyed;
        std::atomic<bool>& onCaller;
        std::atomic<unsigned>& runs;
        std::thread::id caller = std::this_thread::get_id();
        Expired(std::atomic<bool>& destroyed, std::atomic<bool>& onCaller, std::atomic<unsigned>& runs)
            : destroyed(destroyed)
            , onCaller(onCaller)
            , runs(runs)
        {}
        ~Expired() override
        {
            onCaller.store(std::this_thread::get_id() == caller, std::memory_order_relaxed);
            destroyed.store(true, std::memory_order_release);
        }
        rpc::RpcResult<int> run(rpc::OperationContext&) override
        {
            ++runs;
            return {CL_SUCCESS, false, 0};
        }
    };
    std::atomic<bool> destroyed{false}, onCaller{false};
    std::atomic<unsigned> runs{0};
    auto result = registry->adminOwner()
                          ->runtime.submit<int>(std::move(token), std::make_shared<Expired>(destroyed, onCaller, runs))
                          .wait();
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_FALSE(result.may_have_dispatched);
    auto expiry = std::chrono::steady_clock::now() + 1s;
    while(!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < expiry)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(destroyed.load(std::memory_order_acquire));
    EXPECT_FALSE(onCaller.load());
    EXPECT_EQ(runs.load(), 0u);
}

TEST(VisorOwnedRuntime, QueuedMutationObservesQuiescenceBeforeTakingTheMetadataLock)
{
    auto actual = std::make_shared<rpc::RpcRuntime>(32);
    auto registry = KeeperRegistry::CreateOwned({}, actual);
    VisorClientPortal portal;
    auto core = portal.core();
    core->registry = registry;
    std::unique_lock<std::mutex> held(core->operationMutex);
    struct Queued final: rpc::OwnedRpcWork<std::shared_ptr<VisorControlReply>>
    {
        std::shared_ptr<VisorClientPortal::Core> core;
        std::atomic<bool>& entered;
        Queued(std::shared_ptr<VisorClientPortal::Core> core, std::atomic<bool>& entered)
            : core(std::move(core))
            , entered(entered)
        {}
        rpc::RpcResult<std::shared_ptr<VisorControlReply>> run(rpc::OperationContext& operation) override
        {
            entered.store(true, std::memory_order_release);
            auto reply = core->Execute(VisorControlAction::Connect, 1234, "", "", 0, 3, operation.deadline());
            return {reply->status, false, std::move(reply)};
        }
    };
    std::atomic<bool> entered{false};
    auto deadline = rpc::RpcDeadline::after(1s);
    auto pending = actual->submit<std::shared_ptr<VisorControlReply>>(actual->reserve(deadline),
                                                                      std::make_shared<Queued>(core, entered));
    auto expiry = std::chrono::steady_clock::now() + 500ms;
    while(!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < expiry)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(entered.load(std::memory_order_acquire));
    portal.ShutdownServices();
    held.unlock();
    auto result = pending.wait();
    ASSERT_TRUE(result.reply);
    EXPECT_EQ((*result.reply)->status, CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(actual->drain(rpc::RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(VisorLifecycleAdmission, TransientProbeCleanupAdmitsOneOriginalStep)
{
    rpc::RpcRuntime actual(32);
    auto deadline = rpc::RpcDeadline::after(100ms);
    std::vector<rpc::RpcReservation> probes;
    for(unsigned n = 0; n != 32; ++n)
    {
        auto slot = actual.reserve(deadline);
        ASSERT_TRUE(slot);
        probes.push_back(std::move(slot));
    }
    auto actions = std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<rpc::LifecycleStep> step;
    std::thread release(
            [&]
            {
                std::this_thread::sleep_for(15ms);
                probes.clear();
            });
    int result = rpc::startLifecycleStepWhenAvailable(actual,
                                                      deadline,
                                                      step,
                                                      actions,
                                                      [actions]
                                                      {
                                                          ++*actions;
                                                          return CL_SUCCESS;
                                                      });
    release.join();
    EXPECT_EQ(result, CL_SUCCESS);
    if(step)
    {
        EXPECT_EQ(rpc::waitLifecycleStep(step, deadline), CL_SUCCESS);
    }
    EXPECT_EQ(actions->load(), 1u);
    EXPECT_EQ(actual.drain(rpc::RpcDeadline::after(1s)), CL_SUCCESS);
}
TEST(VisorLifecycleAdmission, PermanentCapacityConsumesOriginalCallerBudgetWithoutAdmission)
{
    rpc::RpcRuntime actual(32);
    auto deadline = rpc::RpcDeadline::after(80ms);
    std::vector<rpc::RpcReservation> held;
    for(unsigned n = 0; n != 32; ++n)
    {
        auto slot = actual.reserve(deadline);
        ASSERT_TRUE(slot);
        held.push_back(std::move(slot));
    }
    auto actions = std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<rpc::LifecycleStep> step;
    auto entered = std::chrono::steady_clock::now();
    int result = rpc::startLifecycleStepWhenAvailable(actual,
                                                      deadline,
                                                      step,
                                                      actions,
                                                      [actions]
                                                      {
                                                          ++*actions;
                                                          return CL_SUCCESS;
                                                      });
    EXPECT_EQ(result, CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_GE(std::chrono::steady_clock::now() - entered, 60ms);
    EXPECT_LT(std::chrono::steady_clock::now() - entered, 250ms);
    EXPECT_FALSE(step);
    EXPECT_EQ(actions->load(), 0u);
    held.clear();
    EXPECT_EQ(actual.drain(rpc::RpcDeadline::after(1s)), CL_SUCCESS);
}
TEST(VisorLifecycleAdmission, RunningOriginalStepIsNotReplacedByAnotherCaller)
{
    rpc::RpcRuntime actual(32);
    struct State
    {
        std::atomic<unsigned> started{0}, replacement{0};
        std::atomic<bool> release{false};
    };
    auto state = std::make_shared<State>();
    std::shared_ptr<rpc::LifecycleStep> step;
    ASSERT_EQ(rpc::startLifecycleStepWhenAvailable(actual,
                                                   rpc::RpcDeadline::after(1s),
                                                   step,
                                                   state,
                                                   [state]
                                                   {
                                                       ++state->started;
                                                       while(!state->release) std::this_thread::sleep_for(1ms);
                                                       return CL_SUCCESS;
                                                   }),
              CL_SUCCESS);
    auto original = step;
    auto expiry = std::chrono::steady_clock::now() + 500ms;
    while(!state->started && std::chrono::steady_clock::now() < expiry) std::this_thread::sleep_for(1ms);
    auto caller = rpc::RpcDeadline::after(20ms);
    EXPECT_EQ(rpc::startLifecycleStepWhenAvailable(actual,
                                                   caller,
                                                   step,
                                                   state,
                                                   [state]
                                                   {
                                                       ++state->replacement;
                                                       return CL_SUCCESS;
                                                   }),
              CL_SUCCESS);
    EXPECT_EQ(rpc::waitLifecycleStep(step, caller), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(step, original);
    EXPECT_EQ(state->started.load(), 1u);
    EXPECT_EQ(state->replacement.load(), 0u);
    state->release = true;
    EXPECT_EQ(rpc::waitLifecycleStep(step, rpc::RpcDeadline::after(1s)), CL_SUCCESS);
    EXPECT_EQ(actual.drain(rpc::RpcDeadline::after(1s)), CL_SUCCESS);
}
