#include "admin_test_fixture.h"
#include <private/LifecycleStep.h>
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
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
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
TEST_P(AdminDeadline, RemoteCapacityErrorAfterPartialSetupRetainsAcquisition)
{
    delays[1] = delays[2] = 0;
    replies[4] = CL_ERR_RPC_CAPACITY;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_RPC_CAPACITY);
    auto duplicate = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(duplicate.getErrorCode(), CL_ERR_ACQUIRED);
    EXPECT_TRUE(duplicate.getKeepers().empty());
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
}
TEST_P(AdminDeadline, RemoteDeadlineErrorAfterPartialSetupRetainsAcquisition)
{
    delays[1] = delays[2] = 0;
    replies[4] = CL_ERR_DEADLINE_EXCEEDED;
    VisorClientPortal portal;
    attach(portal);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_DEADLINE_EXCEEDED);
    auto duplicate = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(duplicate.getErrorCode(), CL_ERR_ACQUIRED);
    EXPECT_TRUE(duplicate.getKeepers().empty());
    for(auto const& count: stops) EXPECT_EQ(count.load(), 0);
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
