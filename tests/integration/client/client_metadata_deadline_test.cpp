#include <chronolog_client.h>
#include "ClientNativePeer.h"
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include <ConnectResponseMsg.h>
#include <AcquireStoryResponseMsg.h>
#include <gtest/gtest.h>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <future>
#include <filesystem>
#include <thread>

namespace chronolog
{
struct ClientRpcContextTestAccess
{
    static void beforeInitializationUnlock(ClientRpcContext& context, std::function<void()> observer)
    {
        context.beforeInitializeUnlock_ = std::move(observer);
    }
};
} // namespace chronolog
namespace
{
using namespace chronolog;
using Clock = std::chrono::steady_clock;
size_t threadCount()
{
    return static_cast<size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                                             std::filesystem::directory_iterator{}));
}
class MetadataPeer: public test::NativePeer
{
public:
    explicit MetadataPeer(std::string const& protocol)
        : NativePeer(protocol)
    {}
};
// Only a definite pre-dispatch expiry may be retried. Wait for the actual
// initialization attempt to settle first; never retry an ambiguous Connect.
int connectReady(std::shared_ptr<ChronologClientImpl> const& owner)
{
    int status = owner->Connect();
    if(status == CL_ERR_DEADLINE_EXCEEDED)
    {
        if(!test::until([&] { return owner->context()->initializationSettled.load(); }))
            return status;
        status = owner->Connect();
    }
    return status;
}
int connectReady(Client& client, ClientPortalServiceConf const& portal)
{
    auto owner = ChronologClientImpl::GetClientImplInstance(portal, WRITER_MODE, ClientQueryServiceConf{});
    int status = client.Connect();
    if(status == CL_ERR_DEADLINE_EXCEEDED)
    {
        if(!test::until([&] { return owner->context()->initializationSettled.load(); }))
            return status;
        status = client.Connect();
    }
    return status;
}
} // namespace
class ClientMetadataDeadline: public ::testing::TestWithParam<char const*>
{
};
TEST_P(ClientMetadataDeadline, DelayedMutationReturnsUnconfirmedWithinControlBudget)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().delay = true;
    auto started = Clock::now();
    auto status = client.CreateChronicle("delayed");
    auto elapsed = Clock::now() - started;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, status);
    EXPECT_LT(elapsed, std::chrono::milliseconds(120));
    peer.state().delay = false;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.Disconnect());
}

TEST_P(ClientMetadataDeadline, RemoteCapacityAcquireStaysUnconfirmedWithoutRetryOrHandle)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().acquireStatus = CL_ERR_RPC_CAPACITY;
    auto first = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, first.first);
    EXPECT_EQ(nullptr, first.second);
    auto second = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, second.first);
    EXPECT_EQ(nullptr, second.second);
    EXPECT_EQ(1u, peer.state().acquisitions.load());
}
TEST_P(ClientMetadataDeadline, OldAlreadyAcquiredWithoutRosterStaysUnconfirmed)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().acquireStatus = CL_ERR_ACQUIRED;
    auto first = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, first.first);
    EXPECT_EQ(nullptr, first.second);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.DestroyStory("c", "s"));
    EXPECT_EQ(0u, peer.state().destroys.load());
}
TEST_P(ClientMetadataDeadline, LostDestroyReplyCannotRetryAgainstRecreatedName)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().delay = true;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.DestroyStory("c", "s"));
    peer.state().delay = false;
    peer.state().storyGeneration = 2;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.AcquireStory("c", "s").first);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.DestroyStory("c", "s"));
    EXPECT_EQ(1u, peer.state().destroys.load());
    EXPECT_EQ(1u, peer.state().destroyedGeneration.load());
}
TEST_P(ClientMetadataDeadline, ListTimeoutReturnsNoLateOutput)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().delay = true;
    auto result = client.ShowChronicles();
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.first);
    EXPECT_TRUE(result.second.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(result.second.empty());
}

TEST_P(ClientMetadataDeadline, MultipleWrappersAndCopiesRetainActualContext)
{
    MetadataPeer peer(GetParam());
    auto original = std::make_unique<Client>(peer.portal);
    Client copy(*original);
    Client other(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(*original, peer.portal));
    original.reset();
    EXPECT_EQ(CL_SUCCESS, other.CreateChronicle("still-owned"));
    EXPECT_EQ(CL_SUCCESS, copy.Disconnect());
    EXPECT_EQ(CL_SUCCESS, other.Connect());
    EXPECT_EQ(CL_SUCCESS, other.Disconnect());
}
TEST_P(ClientMetadataDeadline, ConcurrentCallersUseIndependentEntryBudgets)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().delay = true;
    auto first = std::async(std::launch::async, [&] { return client.CreateChronicle("first"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto started = Clock::now();
    auto second = client.CreateChronicle("second");
    auto elapsed = Clock::now() - started;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, first.get());
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, second);
    EXPECT_LT(elapsed, std::chrono::milliseconds(120));
}
TEST_P(ClientMetadataDeadline, RemoteDeadlineAcquireStaysUnconfirmed)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().acquireStatus = CL_ERR_DEADLINE_EXCEEDED;
    auto result = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, result.first);
    EXPECT_EQ(nullptr, result.second);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.ReleaseStory("c", "s"));
    EXPECT_EQ(1u, peer.state().acquisitions.load());
}
TEST_P(ClientMetadataDeadline, SuccessWithoutRosterDoesNotFabricateHandle)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    auto result = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, result.first);
    EXPECT_EQ(nullptr, result.second);
}
TEST_P(ClientMetadataDeadline, ConfirmedLocalDuplicateReturnsSameHandleWithoutRpc)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().roster = true;
    auto first = client.AcquireStory("c", "s");
    ASSERT_EQ(CL_SUCCESS, first.first);
    ASSERT_NE(nullptr, first.second);
    auto second = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_SUCCESS, second.first);
    EXPECT_EQ(first.second, second.second);
    EXPECT_EQ(1u, peer.state().acquisitions.load());
    EXPECT_EQ(CL_SUCCESS, client.ReleaseStory("c", "s"));
    EXPECT_EQ(CL_SUCCESS, client.Disconnect());
}
TEST_P(ClientMetadataDeadline, DisconnectDuringUnconfirmedAcquireDoesNotReuseIdentity)
{
    MetadataPeer peer(GetParam());
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    peer.state().delay = true;
    auto acquired = client.AcquireStory("c", "s");
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, acquired.first);
    EXPECT_EQ(nullptr, acquired.second);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.Disconnect());
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.AcquireStory("c", "s").first);
    EXPECT_EQ(1u, peer.state().acquisitions.load());
}

TEST_P(ClientMetadataDeadline, HealthyAcquireReleaseChurnDoesNotRetainInactiveHandles)
{
    MetadataPeer peer(GetParam());
    peer.state().roster = true;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, connectReady(owner));
    for(unsigned i = 0; i < 100; ++i)
    {
        auto acquired = owner->AcquireStory("churn", "story");
        ASSERT_EQ(CL_SUCCESS, acquired.first);
        ASSERT_NE(nullptr, acquired.second);
        ASSERT_EQ(CL_SUCCESS, owner->ReleaseStory("churn", "story"));
        EXPECT_EQ(0u, owner->context()->storyteller->retainedHandleCount());
    }
    auto context = owner->context();
    std::weak_ptr<thallium::engine> engine = context->engine;
    owner.reset();
    EXPECT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_TRUE(engine.expired());
}
// These internal stress cases pause after native admission. They check extra
// lease retention, not safety of arbitrary unsynchronized raw invocation.
TEST_P(ClientMetadataDeadline, InternalReleaseRetainsAdmittedNativeCallThenReclaimsHandle)
{
    MetadataPeer peer(GetParam());
    peer.state().roster = true;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, connectReady(owner));
    auto acquired = owner->AcquireStory("held", "story");
    ASSERT_EQ(CL_SUCCESS, acquired.first);
    peer.state().blockRecords = true;
    auto logged = std::async(std::launch::async, [handle = acquired.second] { return handle->log_event("held"); });
    struct Unblock
    {
        std::atomic<bool>& flag;
        ~Unblock() { flag = false; }
    } unblock{peer.state().blockRecords};
    ASSERT_TRUE(peer.until([&] { return peer.state().records.load() == 1; }));
    EXPECT_EQ(CL_SUCCESS, owner->ReleaseStory("held", "story"));
    EXPECT_EQ(1u, owner->context()->storyteller->retainedHandleCount());
    peer.state().blockRecords = false;
    EXPECT_NE(0u, logged.get());
    EXPECT_EQ(0u, owner->context()->storyteller->retainedHandleCount());
    auto context = owner->context();
    owner.reset();
    EXPECT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
}
TEST_P(ClientMetadataDeadline, InternalFacadeDropRetainsActualEngineDuringAdmittedNativeCall)
{
    MetadataPeer peer(GetParam());
    peer.state().roster = true;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, connectReady(owner));
    auto acquired = owner->AcquireStory("held", "story");
    ASSERT_EQ(CL_SUCCESS, acquired.first);
    auto context = owner->context();
    std::weak_ptr<thallium::engine> engine = context->engine;
    peer.state().blockRecords = true;
    auto logged = std::async(std::launch::async, [handle = acquired.second] { return handle->log_event("held"); });
    struct Unblock
    {
        std::atomic<bool>& flag;
        ~Unblock() { flag = false; }
    } unblock{peer.state().blockRecords};
    ASSERT_TRUE(peer.until([&] { return peer.state().records.load() == 1; }));
    auto start = Clock::now();
    owner.reset();
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(120));
    EXPECT_FALSE(context->cleanupComplete.load());
    EXPECT_FALSE(engine.expired());
    auto rejected = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    EXPECT_EQ(context.get(), rejected->context().get());
    EXPECT_EQ(nullptr, rejected->runtime());
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, rejected->Connect());
    peer.state().blockRecords = false;
    EXPECT_NE(0u, logged.get());
    EXPECT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_TRUE(engine.expired());
}
TEST_P(ClientMetadataDeadline, UnknownMutationKeepsActualSingletonAfterFacadeDrop)
{
    MetadataPeer peer(GetParam());
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, connectReady(owner));
    peer.state().delay = true;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, owner->DestroyStory("unknown", "story"));
    auto context = owner->context();
    std::weak_ptr<thallium::engine> engine = context->engine;
    owner.reset();
    auto rejected = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    EXPECT_EQ(context.get(), rejected->context().get());
    EXPECT_EQ(nullptr, rejected->runtime());
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, rejected->Connect());
    EXPECT_FALSE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_FALSE(engine.expired());
    EXPECT_FALSE(context->cleanupComplete.load());
    EXPECT_EQ(1u, peer.state().destroys.load());
}
TEST_P(ClientMetadataDeadline, ConfirmedRemoteCapacityReplyKeepsUnresolvedSingletonAfterFacadeDrop)
{
    MetadataPeer peer(GetParam());
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, connectReady(owner));
    peer.state().acquireStatus = CL_ERR_RPC_CAPACITY;
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, owner->AcquireStory("unknown", "story").first);
    auto context = owner->context();
    std::weak_ptr<thallium::engine> engine = context->engine;
    owner.reset();
    EXPECT_FALSE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_FALSE(engine.expired());
    auto rejected = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    EXPECT_EQ(context.get(), rejected->context().get());
    EXPECT_EQ(nullptr, rejected->runtime());
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, rejected->Connect());
}
TEST_P(ClientMetadataDeadline, AmbiguousConnectBlocksLifecycleAndMutationsWithoutRetry)
{
    MetadataPeer peer(GetParam());
    peer.state().delayConnect = true;
    Client client(peer.portal);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, connectReady(client, peer.portal));
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.Connect());
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.Disconnect());
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.CreateChronicle("blocked"));
    EXPECT_EQ(1u, peer.state().connects.load());
    EXPECT_EQ(0u, peer.state().disconnects.load());
    EXPECT_EQ(0u, client.client_id());
}
TEST_P(ClientMetadataDeadline, AmbiguousDisconnectPreservesIdentityAndAcquiredHandleState)
{
    MetadataPeer peer(GetParam());
    peer.state().roster = true;
    Client client(peer.portal);
    ASSERT_EQ(CL_SUCCESS, connectReady(client, peer.portal));
    ASSERT_EQ(CL_SUCCESS, client.AcquireStory("held", "story").first);
    auto identity = client.client_id();
    peer.state().delayDisconnect = true;
    auto started = Clock::now();
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.Disconnect());
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_EQ(identity, client.client_id());
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.AcquireStory("held", "story").first);
    EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, client.DestroyStory("held", "story"));
    EXPECT_EQ(1u, peer.state().disconnects.load());
    EXPECT_EQ(1u, peer.state().acquisitions.load());
    EXPECT_EQ(0u, peer.state().destroys.load());
}
TEST_P(ClientMetadataDeadline, ColdStartupExpiryDoesNotDispatchAndOwnerDropRetiresNativeWork)
{
    MetadataPeer peer(GetParam());
    auto conf = peer.portal;
    conf.RPC_POLICY.control_ms = 40;
    auto owner = ChronologClientImpl::GetClientImplInstance(conf, WRITER_MODE, ClientQueryServiceConf{});
    auto context = owner->context();
    auto start = Clock::now();
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, owner->Connect());
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(120));
    ASSERT_TRUE(peer.until([&] { return context->initializationSettled.load(); }));
    ASSERT_NE(nullptr, context->engine); // Actual native startup continued after expiry.
    start = Clock::now();
    owner.reset();
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(120));
    ASSERT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_EQ(0u, peer.state().connects.load());
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    conf.RPC_POLICY.control_ms = 5000;
    auto next = ChronologClientImpl::GetClientImplInstance(conf, WRITER_MODE, ClientQueryServiceConf{});
    EXPECT_NE(context.get(), next->context().get());
    EXPECT_EQ(CL_SUCCESS, next->Connect());
    EXPECT_EQ(1u, peer.state().connects.load());
    auto nextContext = next->context();
    next.reset();
    EXPECT_TRUE(peer.until([&] { return nextContext->cleanupComplete.load(); }));
}
TEST_P(ClientMetadataDeadline, HealthyCleanupAllowsFreshOwnedNativeGeneration)
{
    MetadataPeer peer(GetParam());
    for(unsigned generation = 0; generation < 2; ++generation)
    {
        auto conf = peer.portal;
        conf.RPC_POLICY.control_ms = 5000;
        auto owner = ChronologClientImpl::GetClientImplInstance(conf, WRITER_MODE, ClientQueryServiceConf{});
        ASSERT_EQ(CL_SUCCESS, connectReady(owner));
        size_t stackSize = 0;
        ASSERT_EQ(ABT_SUCCESS, ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &stackSize));
        EXPECT_EQ(2097152u, stackSize);
        auto context = owner->context();
        std::weak_ptr<thallium::engine> engine = context->engine;
        EXPECT_EQ(CL_SUCCESS, owner->Disconnect());
        owner.reset();
        ASSERT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
        EXPECT_TRUE(engine.expired());
        EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
        EXPECT_TRUE(peer.until([] { return threadCount() == 3; }));
    }
    EXPECT_EQ(2u, peer.state().connects.load());
}
TEST_P(ClientMetadataDeadline, ReplayAdmissionExpiresWhileFirstReaderInitializationOwnsState)
{
    MetadataPeer peer(GetParam());
    auto conf = peer.portal;
    conf.RPC_POLICY.replay_ms = 40;
    ClientQueryServiceConf query{GetParam(), "127.0.0.1", 0, 75};
    Client client(conf, query);
    auto owner = ChronologClientImpl::GetClientImplInstance(conf, READER_MODE, query);
    struct Gate
    {
        std::atomic<bool> entered{false}, open{false};
        void wait()
        {
            entered = true;
            while(!open.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto gate = std::make_shared<Gate>();
    struct OpenOnExit
    {
        std::shared_ptr<Gate> gate;
        ~OpenOnExit() { gate->open = true; }
    } cleanup{gate};
    ClientRpcContextTestAccess::beforeInitializationUnlock(*owner->context(), [gate] { gate->wait(); });
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, client.Connect());
    ASSERT_TRUE(peer.until([&] { return gate->entered.load(); }));
    EXPECT_FALSE(owner->context()->initializationSettled.load());
    EXPECT_EQ(0u, peer.state().connects.load());
    std::vector<Event> events;
    auto started = Clock::now();
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, client.ReplayStory("c", "s", 0, 1, events));
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_TRUE(events.empty());
    unsigned callbacks = 0;
    Client::EventCallback callback = [&](Event const&) { ++callbacks; };
    started = Clock::now();
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, client.ReplayStory("c", "s", 0, 1, callback));
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_EQ(0u, callbacks);
    gate->open = true;
    ASSERT_TRUE(peer.until([&] { return owner->context()->initializationSettled.load(); }));
    ASSERT_EQ(CL_SUCCESS, client.Connect()); // The first Connect definitely did not dispatch.
    EXPECT_EQ(CL_ERR_NOT_ACQUIRED, client.ReplayStory("c", "s", 0, 1, events));
    EXPECT_EQ(CL_ERR_NOT_ACQUIRED, client.ReplayStory("c", "s", 0, 1, callback));
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(0u, callbacks);
}
TEST_P(ClientMetadataDeadline, ReaderCleanupStaysChargedWithoutIncomingQuiescenceProof)
{
    MetadataPeer peer(GetParam());
    auto conf = peer.portal;
    conf.RPC_POLICY.control_ms = 5000;
    ClientQueryServiceConf query{GetParam(), "127.0.0.1", 0, 75};
    auto owner = ChronologClientImpl::GetClientImplInstance(conf, READER_MODE, query);
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto context = owner->context();
    ASSERT_NE(nullptr, context->reader);
    auto runtime = owner->runtime();
    std::weak_ptr<thallium::engine> engine = context->engine;
    owner.reset();
    EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(rpc::RpcDeadline::after(std::chrono::milliseconds(40))));
    EXPECT_FALSE(context->cleanupComplete.load());
    EXPECT_FALSE(engine.expired());
    auto rejected = ChronologClientImpl::GetClientImplInstance(conf, READER_MODE, query);
    EXPECT_EQ(context.get(), rejected->context().get());
    EXPECT_EQ(nullptr, rejected->runtime());
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, rejected->Connect());
}
TEST_P(ClientMetadataDeadline, PreexistingMargoIsRejectedWithoutChangingItsNativeLifetime)
{
    MetadataPeer peer(GetParam());
    auto external = margo_init(GetParam(), MARGO_CLIENT_MODE, 1, 1);
    ASSERT_NE(MARGO_INSTANCE_NULL, external);
    std::shared_ptr<ClientRpcContext> context;
    {
        auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
        context = owner->context();
        EXPECT_EQ(CL_ERR_NATIVE_RUNTIME_UNSUPPORTED, owner->Connect());
        EXPECT_EQ(0u, peer.state().connects.load());
        EXPECT_EQ(ABT_SUCCESS, ABT_initialized());
    }
    ASSERT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
    margo_finalize_and_wait(external);
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    EXPECT_EQ(0u, peer.state().connects.load());
}
TEST_P(ClientMetadataDeadline, PreexistingArgobotsIsRejectedBeforeConnectDispatch)
{
    MetadataPeer peer(GetParam());
    ASSERT_EQ(ABT_SUCCESS, ABT_init(0, nullptr));
    std::shared_ptr<ClientRpcContext> context;
    {
        auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
        context = owner->context();
        Client client(peer.portal);
        EXPECT_EQ(CL_ERR_NATIVE_RUNTIME_UNSUPPORTED, client.Connect());
        EXPECT_EQ(0u, peer.state().connects.load());
    }
    ASSERT_TRUE(peer.until([&] { return context->cleanupComplete.load(); }));
    EXPECT_EQ(ABT_SUCCESS, ABT_finalize());
    Client later(peer.portal);
    EXPECT_EQ(CL_ERR_NATIVE_RUNTIME_UNSUPPORTED, later.Connect());
    EXPECT_EQ(0u, peer.state().connects.load());
}
INSTANTIATE_TEST_SUITE_P(Native, ClientMetadataDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
