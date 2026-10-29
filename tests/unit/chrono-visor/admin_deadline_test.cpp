#include "admin_test_fixture.h"
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
        EXPECT_EQ(lease->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
    gate->released.store(true, std::memory_order_release);
    EXPECT_TRUE(eventually(
            [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
    EXPECT_EQ(probe->finalizers.load(), 1u);
    for(auto const& count: shutdownEntries) EXPECT_EQ(count.load(), 0u);
}
TEST_P(RegistryShutdownOwnership, ShutdownNoReplyRetainsActualOwnerAfterWorkerReturn)
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
    EXPECT_TRUE(eventually([&]() { return observation->shutdownNoReplyRunReturns.load() != 0; }));
    EXPECT_EQ(probe->finalizers.load(), 0u);
    EXPECT_GT(observation->shutdownExceptionKind.load(), 0);
    std::cout << "native_shutdown_exception_kind=" << observation->shutdownExceptionKind.load()
              << " hg_error=" << observation->nativeShutdownError.load() << " name="
              << tl::translate_margo_error_code(static_cast<hg_return_t>(observation->nativeShutdownError.load()))
              << std::endl;
    registry.reset();
    EXPECT_EQ(result.load(), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_FALSE(probe->registry.expired());
    EXPECT_FALSE(probe->provider.expired());
    EXPECT_FALSE(probe->engine.expired());
    EXPECT_EQ(probe->finalizers.load(), 0u);
    if(auto lease = probe->registry.lock())
        EXPECT_EQ(lease->ShutdownRegistryService(), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_FALSE(probe->registry.expired());
    EXPECT_EQ(probe->finalizers.load(), 0u);
}
TEST_P(RegistryShutdownOwnership, ShutdownConfirmedReclaimsDirectRegistryAndProvider)
{
    EXPECT_EQ(registry->ShutdownRegistryService(), CL_SUCCESS);
    EXPECT_EQ(registry->ShutdownRegistryService(), CL_SUCCESS);
    registry.reset();
    EXPECT_TRUE(eventually(
            [&]() { return probe->registry.expired() && probe->provider.expired() && probe->engine.expired(); }));
    EXPECT_EQ(probe->finalizers.load(), 1u);
    EXPECT_EQ(shutdownEntries[0].load(), 1u);
    for(int i = 1; i <= 3; ++i) EXPECT_EQ(shutdownEntries[i].load(), 1u);
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
