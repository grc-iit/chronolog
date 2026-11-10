#include <gtest/gtest.h>
#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>
#include <private/StopDependencyEvidence.h>
#include <private/RpcProviderRegistration.h>
#include <chrono_monitor.h>
#include <abt.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace chl = chronolog;
namespace tl = thallium;
namespace chronolog
{
struct WatermarkRegistryTestAccess
{
    static std::unique_lock<std::timed_mutex> lock(StoryWatermarkRegistry& registry)
    {
        return std::unique_lock<std::timed_mutex>(registry.mtx);
    }
};
} // namespace chronolog
namespace chronolog::rpc
{
struct StopPublicationTestAccess
{
    static PublisherParkProof proof(ServiceId const& keeper, std::string identity = "same-generation-drain")
    {
        std::string target;
        keeper.get_service_as_string(target);
        return {target + "@" + std::to_string(keeper.getProviderId()), std::move(identity)};
    }
};
} // namespace chronolog::rpc
namespace
{
using namespace std::chrono_literals;
bool waitFor(std::function<bool()> const& predicate, std::chrono::milliseconds budget = 3000ms)
{
    auto expiry = std::chrono::steady_clock::now() + budget;
    while(!predicate())
    {
        if(std::chrono::steady_clock::now() >= expiry)
            return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
struct Gate
{
    std::atomic<unsigned> entered{0};
    std::atomic<bool> release{false}, pause{false};
};
struct ReleaseGate
{
    std::shared_ptr<Gate> gate;
    ~ReleaseGate() { gate->release = true; }
};
struct Received
{
    std::atomic<unsigned> entered{0};
    std::atomic<bool> hold{false};
    std::mutex mutex;
    std::vector<std::map<chl::StoryId, chl::StoryWatermarkReport>> maps;
    std::size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return maps.size();
    }
    chl::StoryWatermarkReport latest(chl::StoryId story = 5)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return maps.back().at(story);
    }
};
class Receiver: public tl::provider<Receiver>
{
    margo_instance_id mid;
    std::shared_ptr<Received> state;

public:
    Receiver(tl::engine& engine, uint16_t provider, std::shared_ptr<Received> state)
        : tl::provider<Receiver>(engine, provider)
        , mid(engine.get_margo_instance())
        , state(std::move(state))
    {
        define("report_story_watermarks", &Receiver::report, tl::ignore_return_value());
    }
    void report(tl::request const&, std::map<chl::StoryId, chl::StoryWatermarkReport> const& reports)
    {
        ++state->entered;
        while(state->hold) margo_thread_sleep(mid, 1);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->maps.push_back(reports);
    }
};
struct PeerRoot
{
    std::shared_ptr<Gate> monitor;
    std::shared_ptr<tl::engine> engine;
    std::vector<std::shared_ptr<Receiver>> providers;
};
bool idle(std::shared_ptr<chl::rpc::RpcRuntime> const& runtime)
{
    auto deadline = chl::rpc::RpcDeadline::after(20ms);
    auto first = runtime->reserve(deadline);
    if(!first)
        return false;
    auto second = runtime->reserve(deadline);
    return bool(second);
}
void initialize()
{
    static bool once = []
    {
        // Match the native Margo profile before the first ABI initialization.
        // Applying its environment after ABT_init leaves 16 KiB ULT stacks.
        if(margo_set_environment(nullptr) != 0 || ABT_init(0, nullptr) != ABT_SUCCESS)
            throw std::runtime_error("Could not initialize the cold Margo fixture profile");
        std::size_t stackSize = 0;
        if(ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &stackSize) != ABT_SUCCESS)
            throw std::runtime_error("Could not inspect native ULT stack size");
        std::cout << "Watermark fixture actual default ULT stack bytes=" << stackSize << std::endl;
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "watermark_deadline_test");
        return true;
    }();
    (void)once;
}
class WatermarkRpcDeadline: public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        initialize();
        nativeGate = std::make_shared<Gate>();
        margo_monitor monitor{};
        monitor.uargs = nativeGate.get();
        monitor.initialize = [](margo_instance_id, void* data, json_object*) -> void* { return data; };
        monitor.on_wait = [](void* data, double, margo_monitor_event_t event, margo_monitor_wait_args_t)
        {
            auto& gate = *static_cast<Gate*>(data);
            if(event != MARGO_MONITOR_FN_START || !gate.pause)
                return;
            ++gate.entered;
            while(!gate.release) std::this_thread::sleep_for(1ms);
        };
        margo_init_info info{};
        info.progress_pool = ABT_POOL_NULL;
        info.rpc_pool = ABT_POOL_NULL;
        info.json_config = "{\"use_progress_thread\":true,\"rpc_thread_count\":1}";
        info.monitor = &monitor;
        auto mid = margo_init_ext((std::string(GetParam()) + "://127.0.0.1").c_str(), MARGO_SERVER_MODE, &info);
        ASSERT_NE(mid, MARGO_INSTANCE_NULL);
        // This fixture creates the MID and transfers authority to owner at start;
        // it never copies a wrapper or finalizes a retained MID from TearDown.
        engine = std::make_shared<tl::engine>(mid);
    }
    void TearDown() override
    {
        if(engine && !retained)
            engine->finalize();
    }
    void start(unsigned targets = 1, chl::RpcTimeoutPolicy policy = {}, uint32_t interval = 0)
    {
        retained = true;
        runtime = std::make_shared<chl::rpc::RpcRuntime>();
        owner = chl::rpc::RpcBulkOwner::Create(engine, runtime, policy, false);
        registry = std::make_shared<chl::StoryWatermarkRegistry>();
        root = std::make_shared<PeerRoot>();
        root->monitor = nativeGate;
        root->engine =
                std::make_shared<tl::engine>(std::string(GetParam()) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        owner->dependencies = root;
        publisher = std::make_unique<chl::WatermarkReportPublisher>(owner, registry, interval);
        std::string self = root->engine->self();
        auto port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        registry->registerStory(5, 100);
        registry->advancePersisted(5, 100, 200);
        for(unsigned n = 0; n != targets; ++n)
        {
            auto state = std::make_shared<Received>();
            received.push_back(state);
            root->providers.push_back(std::make_shared<Receiver>(*root->engine, 71 + n, state));
            chl::ServiceId endpoint(GetParam(), "127.0.0.1", port, 71 + n);
            endpoints.push_back(endpoint);
            publisher->recordContributor(5, endpoint);
        }
    }
    bool pump(std::function<bool()> const& predicate, std::chrono::milliseconds budget = 3000ms)
    {
        return waitFor(
                [&]
                {
                    publisher->publish(true);
                    return predicate();
                },
                budget);
    }
    bool retained = false;
    std::shared_ptr<Gate> nativeGate;
    std::shared_ptr<tl::engine> engine;
    std::shared_ptr<chl::rpc::RpcRuntime> runtime;
    std::shared_ptr<chl::rpc::RpcBulkOwner> owner;
    std::shared_ptr<chl::StoryWatermarkRegistry> registry;
    std::unique_ptr<chl::WatermarkReportPublisher> publisher;
    std::shared_ptr<PeerRoot> root;
    std::vector<std::shared_ptr<Received>> received;
    std::vector<chl::ServiceId> endpoints;
};
// Catch provider provisioning that silently re-enables replies on a one-way RPC.
TEST_P(WatermarkRpcDeadline, ProviderProvisioningPreservesOneWaySendAndReuse)
{
    auto remote = engine->define("report_story_watermarks").disable_response();
    chl::rpc::RpcProviderRegistration registration(*engine);
    for(unsigned repeat = 0; repeat != 2; ++repeat)
    {
        ASSERT_EQ(registration.provision("report_story_watermarks",
                                         71,
                                         chl::rpc::RpcDeadline::after(std::chrono::seconds(1))),
                  chl::CL_SUCCESS);
        hg_id_t id = 0;
        hg_bool_t registered = HG_FALSE;
        ASSERT_EQ(margo_provider_registered_name(engine->get_margo_instance(),
                                                 "report_story_watermarks",
                                                 71,
                                                 &id,
                                                 &registered),
                  HG_SUCCESS);
        ASSERT_TRUE(registered);
        int disabled = 0;
        ASSERT_EQ(margo_registered_disabled_response(engine->get_margo_instance(), id, &disabled), HG_SUCCESS);
        EXPECT_TRUE(disabled) << "provider-specific one-way RPC must not wait for a nonexistent reply";
    }
}
// Catch losing quiet progress after the fourth definite address-lookup failure.
TEST_P(WatermarkRpcDeadline, QuietProgressSurvivesRepeatedDefiniteFailures)
{
    auto registryOwner = std::make_shared<chl::StoryWatermarkRegistry>();
    auto& registry = *registryOwner;
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>();
    auto owner = chl::rpc::RpcBulkOwner::Create(engine, runtime, {}, false);
    retained = true;
    chl::WatermarkReportPublisher publisher(owner, registryOwner, 0);
    registry.registerStory(5, 100);
    registry.advancePersisted(5, 100, 200);
    publisher.recordContributor(5, chl::ServiceId("unsupported-watermark-protocol", "127.0.0.1", 1, 71));
    for(unsigned attempt = 0; attempt != 4; ++attempt) publisher.publish();
    auto pending = registry.snapshotDirty();
    ASSERT_EQ(pending.count(5), 1u);
    EXPECT_EQ(pending.at(5).watermark, 200u);
}

TEST_P(WatermarkRpcDeadline, ProvisioningRefusesToToggleAnExposedProviderMode)
{
    auto remote = engine->define("mode_immutable");
    chl::rpc::RpcProviderRegistration registration(*engine);
    auto deadline = chl::rpc::RpcDeadline::after(1s);
    ASSERT_EQ(registration.provision("mode_immutable", 71, deadline), chl::CL_SUCCESS);
    remote.disable_response();
    EXPECT_EQ(registration.provision("mode_immutable", 71, deadline), chl::CL_ERR_INVALID_ARG);
    hg_id_t id = 0;
    hg_bool_t registered = HG_FALSE;
    int disabled = 1;
    ASSERT_EQ(margo_provider_registered_name(engine->get_margo_instance(), "mode_immutable", 71, &id, &registered),
              HG_SUCCESS);
    ASSERT_EQ(margo_registered_disabled_response(engine->get_margo_instance(), id, &disabled), HG_SUCCESS);
    EXPECT_FALSE(disabled);
}
TEST_P(WatermarkRpcDeadline, ConcurrentFirstAndReusedNativeOneWaySendsReachBothProviders)
{
    start(2);
    ASSERT_TRUE(pump([&] { return received[0]->count() && received[1]->count(); }));
    EXPECT_EQ(received[0]->latest().watermark, 200u);
    registry->advancePersisted(5, 200, 300);
    ASSERT_TRUE(pump([&] { return received[0]->latest().watermark == 300 && received[1]->latest().watermark == 300; }));
    EXPECT_THROW(chl::WatermarkReportPublisher(owner, registry, 0), std::logic_error);
}
TEST_P(WatermarkRpcDeadline, FinalForcedReportIncludesProgressAfterAnEarlierSend)
{
    start(1, {}, 3600);
    ASSERT_TRUE(pump([&] { return received[0]->count() != 0; }));
    registry->advancePersisted(5, 200, 300);
    ASSERT_TRUE(pump([&] { return received[0]->latest().watermark == 300; }, 300ms));
}
// The old stop loop calls publish(force=true) on every attempt. A known
// unchanged local completion must not manufacture another native send.
TEST_P(WatermarkRpcDeadline, RepeatedStopPassDoesNotResendUnchangedFinalEnvelope)
{
    start(1, {}, 3600);
    EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1 && idle(runtime); }));
    // Reservation destruction returns fixed slots on the existing worker poll.
    std::this_thread::sleep_for(30ms);
    for(unsigned attempt = 0; attempt != 3; ++attempt)
    {
        EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
        ASSERT_TRUE(waitFor([&] { return idle(runtime); }));
        std::this_thread::sleep_for(30ms);
    }
    EXPECT_EQ(received[0]->count(), 1u);
}
TEST_P(WatermarkRpcDeadline, ChangedFinalEnvelopeUsesTheSameOwnerAndLiveCompletionIsNotExitProof)
{
    start(1, {}, 3600);
    EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1 && idle(runtime); }));
    std::this_thread::sleep_for(30ms);
    registry->advancePersisted(5, 200, 300);
    EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 2 && idle(runtime); }));
    EXPECT_EQ(received[0]->latest().watermark, 300u);
    EXPECT_EQ(publisher->finish(chl::rpc::RpcDeadline::after(1s), {}), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
}
TEST_P(WatermarkRpcDeadline, ParkCoversOrdinaryAndForcedDispatchAfterTheKeeperReadinessBoundary)
{
    start();
    auto proof = chl::rpc::StopPublicationTestAccess::proof(endpoints[0]);
    EXPECT_EQ(publisher->park({}, chl::rpc::RpcDeadline::after(1s)), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(publisher->park(proof, chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    for(unsigned pass = 0; pass != 3; ++pass)
    {
        EXPECT_EQ(publisher->publish(false), chl::CL_SUCCESS);
        EXPECT_EQ(publisher->publish(true), chl::CL_SUCCESS);
        EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    }
    ASSERT_TRUE(idle(runtime));
    EXPECT_EQ(received[0]->count(), 0u);
    // Retry the stable drain identity, not the caller attempt number.
    EXPECT_EQ(publisher->park(proof, chl::rpc::RpcDeadline::after(1s)), chl::CL_SUCCESS);
    EXPECT_EQ(publisher->park(chl::rpc::StopPublicationTestAccess::proof(endpoints[0], "another-drain"),
                              chl::rpc::RpcDeadline::after(1s)),
              chl::CL_ERR_SHUTDOWN_INCOMPLETE);
}
TEST_P(WatermarkRpcDeadline, ParkWaitsForPreadmittedNativePreparationAndPreventsItsForward)
{
    start();
    auto gate = std::make_shared<Gate>();
    ReleaseGate release{gate};
    owner->beforeDispatch = [gate]
    {
        ++gate->entered;
        while(!gate->release) std::this_thread::sleep_for(1ms);
    };
    EXPECT_EQ(publisher->publish(false), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&] { return gate->entered.load() == 1; }));
    auto proof = chl::rpc::StopPublicationTestAccess::proof(endpoints[0]);
    EXPECT_EQ(publisher->park(proof, chl::rpc::RpcDeadline::after(50ms)), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
    gate->release = true;
    ASSERT_TRUE(waitFor([&] { return publisher->park(proof, chl::rpc::RpcDeadline::after(50ms)) == chl::CL_SUCCESS; }));
    ASSERT_TRUE(waitFor([&] { return idle(runtime); }));
    EXPECT_EQ(received[0]->count(), 0u);
}
TEST_P(WatermarkRpcDeadline, ParkDoesNotAcknowledgeAHeldOriginalNativeSend)
{
    start();
    ReleaseGate release{nativeGate};
    nativeGate->pause = true;
    EXPECT_EQ(publisher->publish(false), chl::CL_SUCCESS);
    ASSERT_TRUE(waitFor([&] { return nativeGate->entered.load() == 1; }));
    auto proof = chl::rpc::StopPublicationTestAccess::proof(endpoints[0]);
    EXPECT_EQ(publisher->park(proof, chl::rpc::RpcDeadline::after(30ms)), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
    nativeGate->release = true;
    ASSERT_TRUE(waitFor([&] { return publisher->park(proof, chl::rpc::RpcDeadline::after(50ms)) == chl::CL_SUCCESS; }));
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1 && idle(runtime); }));
    EXPECT_EQ(publisher->publish(true), chl::CL_SUCCESS);
    EXPECT_EQ(received[0]->count(), 1u);
}
// Neither keeper exists, so both reports end Unknown: park and the final pass
// stay incomplete. Both forwards have returned, so both slots come back.
TEST_P(WatermarkRpcDeadline, ParkNeverAcknowledgesUnknownOriginalSends)
{
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 40;
    start(0, policy);
    chl::ServiceId first(GetParam(), "127.0.0.1", 1, 71), second(GetParam(), "127.0.0.1", 2, 72);
    publisher->recordContributor(5, first);
    publisher->recordContributor(5, second);
    EXPECT_EQ(publisher->publish(true), chl::CL_SUCCESS);
    std::this_thread::sleep_for(100ms);
    auto proof = chl::rpc::StopPublicationTestAccess::proof(first);
    EXPECT_EQ(publisher->park(proof, chl::rpc::RpcDeadline::after(50ms)), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(publisher->publishFinal(chl::rpc::RpcDeadline::after(1s)), chl::CL_ERR_OUTCOME_UNCONFIRMED);
    auto one = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    auto two = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    EXPECT_TRUE(one);
    EXPECT_TRUE(two);
}
TEST_P(WatermarkRpcDeadline, LocalSendCompletionDoesNotMeanKeeperApplication)
{
    start();
    received[0]->hold = true;
    struct Resume
    {
        std::shared_ptr<Received> state;
        ~Resume() { state->hold = false; }
    } resume{received[0]};
    publisher->publish(true);
    ASSERT_TRUE(waitFor([&] { return received[0]->entered.load() == 1; }));
    ASSERT_TRUE(waitFor([&] { return idle(runtime); }));
    EXPECT_EQ(received[0]->count(), 0u);
    received[0]->hold = false;
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1; }));
    EXPECT_EQ(received[0]->latest().watermark, 200u);
}
TEST_P(WatermarkRpcDeadline, BeyondEagerMapKeepsEveryPendingReceipt)
{
    start();
    constexpr uint64_t count = 262144;
    for(uint64_t n = 0; n != count; ++n) registry->assignReceipt(5);
    publisher->publish(true);
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1; }, 5000ms));
    auto report = received[0]->latest();
    EXPECT_EQ(report.watermark, 200u);
    EXPECT_EQ(report.highest_receipt, count);
    ASSERT_EQ(report.pending_receipts.size(), count);
    for(uint64_t n = 0; n != count; ++n) ASSERT_EQ(report.pending_receipts[n], n + 1);
}
TEST_P(WatermarkRpcDeadline, CapacityRefusalPreservesProgressAndRotatesDeferredTargets)
{
    start(4);
    auto first = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    auto second = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto registryLock = chl::WatermarkRegistryTestAccess::lock(*registry);
    auto began = std::chrono::steady_clock::now();
    EXPECT_EQ(publisher->publish(true), chl::CL_ERR_RPC_CAPACITY);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 100ms);
    registryLock.unlock();
    EXPECT_EQ(registry->getPersisted(5), 200u);
    for(auto const& state: received) EXPECT_EQ(state->count(), 0u);
    first = {};
    second = {};
    ASSERT_TRUE(pump(
            [&]
            {
                for(auto const& state: received)
                    if(!state->count())
                        return false;
                return true;
            }));
    for(auto const& state: received) EXPECT_EQ(state->latest().watermark, 200u);
}
// With three targets/C2, advancing past a refused target repeats the same
// successful pair forever. Separate settled rounds expose that cursor error.
TEST_P(WatermarkRpcDeadline, ThreeTargetsDoNotStarveTheCapacityRefusedTarget)
{
    start(3);
    for(unsigned round = 0; round != 3; ++round)
    {
        ASSERT_TRUE(waitFor([&] { return idle(runtime); }));
        // idle's temporary reservation cleanup uses the fixed worker poll.
        std::this_thread::sleep_for(20ms);
        publisher->publish(true);
        ASSERT_TRUE(waitFor(
                [&]
                {
                    std::size_t total = 0;
                    for(auto const& state: received) total += state->count();
                    return total >= 2 * (round + 1);
                }));
    }
    for(auto const& state: received) EXPECT_GT(state->count(), 0u);
}
TEST_P(WatermarkRpcDeadline, QueuedPreparationUsesCurrentCompleteEnvelope)
{
    start();
    auto gate = std::make_shared<Gate>();
    ReleaseGate release{gate};
    owner->beforeSerialize = [gate]
    {
        ++gate->entered;
        while(!gate->release) std::this_thread::sleep_for(1ms);
    };
    publisher->publish(true);
    ASSERT_TRUE(waitFor([&] { return gate->entered.load() == 1; }));
    auto settled = registry->assignReceipt(5);
    auto pending = registry->assignReceipt(5);
    registry->receiptMerged(5, settled);
    registry->advancePersisted(5, 200, 400);
    gate->release = true;
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1; }));
    auto report = received[0]->latest();
    EXPECT_EQ(report.watermark, 400u);
    EXPECT_EQ(report.highest_receipt, 2u);
    EXPECT_EQ(report.pending_receipts, std::vector<uint64_t>{pending});
}
// Replay two admitted receive continuations whose receipt/token capture and
// contributor publication straddle drop/recreate. A resumes after B publishes.
TEST_P(WatermarkRpcDeadline, DelayedOldContributorCannotReplaceCurrentIncarnation)
{
    start();
    chl::StoryWatermarkRegistry::Incarnation old;
    ASSERT_EQ(registry->assignReceipt(5, &old), 1u);
    registry->dropStory(5);
    registry->registerStory(5, 300);
    chl::StoryWatermarkRegistry::Incarnation current;
    ASSERT_EQ(registry->assignReceipt(5, &current), 2u);
    ASSERT_EQ(registry->assignReceipt(5), 3u);
    registry->receiptMerged(5, 2);
    publisher->recordContributor(5, endpoints[0], current);
    publisher->recordContributor(5, endpoints[0], old);
    publisher->publish(true);
    // No later chunk/contributor update may be needed to repair quiet progress.
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1; }));
    auto report = received[0]->latest();
    EXPECT_EQ(report.watermark, 300u);
    EXPECT_EQ(report.grapher_instance, registry->instanceId());
    EXPECT_EQ(report.highest_receipt, 3u);
    EXPECT_EQ(report.pending_receipts, std::vector<uint64_t>{3});
}
TEST_P(WatermarkRpcDeadline, ExpiredPreparationNeverDispatchesAndFreshPassKeepsProgress)
{
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 40;
    start(2, policy);
    auto gate = std::make_shared<Gate>();
    ReleaseGate release{gate};
    owner->beforeSerialize = [gate]
    {
        ++gate->entered;
        while(!gate->release) std::this_thread::sleep_for(1ms);
    };
    publisher->publish(chl::rpc::RpcDeadline::after(1000ms), true);
    ASSERT_TRUE(waitFor([&] { return gate->entered.load() == 2; }));
    registry->advancePersisted(5, 200, 300);
    std::this_thread::sleep_for(60ms);
    gate->release = true;
    ASSERT_TRUE(waitFor([&] { return idle(runtime); }));
    EXPECT_EQ(received[0]->count(), 0u);
    EXPECT_EQ(received[1]->count(), 0u);
    ASSERT_TRUE(pump([&] { return received[0]->count() != 0 && received[1]->count() != 0; }));
    EXPECT_EQ(received[0]->latest().watermark, 300u);
}
TEST_P(WatermarkRpcDeadline, QuietProgressRetriesDefiniteFailuresBeyondThreeRounds)
{
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 50;
    start(1, policy);
    auto tries = std::make_shared<std::atomic<unsigned>>(0);
    owner->beforeSerialize = [tries]
    {
        if(++*tries <= 4)
            throw std::runtime_error("definite preparation failure");
    };
    ASSERT_TRUE(pump([&] { return received[0]->count() != 0; }));
    EXPECT_GE(tries->load(), 5u);
    EXPECT_EQ(received[0]->latest().watermark, 200u);
}
TEST_P(WatermarkRpcDeadline, DroppingFacadesRetainsActualEnginesRegistryAndCallable)
{
    start();
    auto gate = nativeGate;
    ReleaseGate release{gate};
    gate->pause = true;
    std::weak_ptr<tl::engine> native = engine;
    std::weak_ptr<chl::StoryWatermarkRegistry> registryWeak = registry;
    publisher->publish(true);
    // Public Margo monitoring enters wait only after native forward submission
    // and input serialization. It holds the original native call continuation;
    // it does not claim that bytes are physically still in flight.
    ASSERT_TRUE(waitFor([&] { return gate->entered.load() == 1; }));
    publisher.reset();
    root.reset();
    registry.reset();
    owner.reset();
    runtime.reset();
    engine.reset();
    nativeGate.reset();
    EXPECT_FALSE(native.expired());
    EXPECT_FALSE(registryWeak.expired());
    // This barrier owns only flags. There is no alternate engine/dependency root.
    gate->release = true;
    ASSERT_TRUE(waitFor([&] { return received[0]->count() == 1; }));
    EXPECT_EQ(received[0]->latest().watermark, 200u);
}
// A report cancelled at its timeout may still reach the keeper, so neither
// target is sent another report. Both forwards have returned, so both slots
// come back.
TEST_P(WatermarkRpcDeadline, UnknownStaleEndpointAttemptsGiveCapacityBackWithoutRetry)
{
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 100;
    start(0, policy);
    auto tries = std::make_shared<std::atomic<unsigned>>(0);
    owner->beforeSerialize = [tries] { ++*tries; };
    publisher->recordContributor(5, chl::ServiceId(GetParam(), "127.0.0.1", 1, 71));
    publisher->recordContributor(5, chl::ServiceId(GetParam(), "127.0.0.1", 2, 72));
    auto began = std::chrono::steady_clock::now();
    publisher->publish(true);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 100ms);
    ASSERT_TRUE(waitFor([&] { return tries->load() == 2; }));
    std::this_thread::sleep_for(150ms);
    for(unsigned n = 0; n != 10; ++n) publisher->publish(true);
    EXPECT_EQ(tries->load(), 2u);
    EXPECT_EQ(registry->getPersisted(5), 200u);
    auto first = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    auto second = runtime->reserve(chl::rpc::RpcDeadline::after(1s));
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
}
TEST_P(WatermarkRpcDeadline, ForcedExpiredPassAndQuiescenceDispatchNothing)
{
    start();
    auto expired = chl::rpc::RpcDeadline::after(1ms);
    std::this_thread::sleep_for(2ms);
    EXPECT_EQ(publisher->publish(expired, true), chl::CL_ERR_DEADLINE_EXCEEDED);
    publisher->quiesce();
    EXPECT_EQ(publisher->publish(true), chl::CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_EQ(received[0]->count(), 0u);
}
TEST(StoryWatermarkPublication, CanonicalTombstoneSurvivesLegacyDirtyConsumptionAndRecreationInvalidatesIt)
{
    initialize();
    auto registry = std::make_shared<chl::StoryWatermarkRegistry>();
    registry->registerStory(5, 100);
    auto old = registry->contributorToken(5);
    registry->dropStory(5);
    registry->snapshotDirty();
    chl::StoryWatermarkRegistry::Publication snapshot;
    ASSERT_EQ(registry->snapshotFor({{5, old}}, chl::rpc::RpcDeadline::after(1s), snapshot), chl::CL_SUCCESS);
    ASSERT_EQ(snapshot.reports.size(), 1u);
    EXPECT_EQ(snapshot.reports.at(5).watermark, chl::kStoryDroppedWatermark);
    registry->registerStory(5, 300);
    struct Validate final: chl::rpc::OwnedRpcWork<int>
    {
        std::shared_ptr<chl::StoryWatermarkRegistry> registry;
        chl::StoryWatermarkRegistry::Publication snapshot;
        chl::rpc::RpcResult<int> run(chl::rpc::OperationContext& operation) override
        {
            int status = registry->beginReport(snapshot, operation);
            return {status, false, static_cast<int>(snapshot.reports.size())};
        }
    };
    auto work = std::make_shared<Validate>();
    work->registry = registry;
    work->snapshot = snapshot;
    chl::rpc::RpcRuntime runtime;
    auto result = runtime.invoke<int>(chl::rpc::RpcDeadline::after(1s), work);
    ASSERT_TRUE(result.reply);
    EXPECT_EQ(*result.reply, 0);
    EXPECT_FALSE(old->load());
    EXPECT_FALSE(snapshot.snapshots.at(5)->load());
    chl::StoryWatermarkRegistry::Publication stale;
    EXPECT_EQ(registry->snapshotFor({{5, old}}, chl::rpc::RpcDeadline::after(1s), stale), chl::CL_SUCCESS);
    EXPECT_TRUE(stale.reports.empty());
    chl::StoryWatermarkRegistry::Publication current;
    ASSERT_EQ(registry->snapshotFor({{5, registry->contributorToken(5)}}, chl::rpc::RpcDeadline::after(1s), current),
              chl::CL_SUCCESS);
    EXPECT_EQ(current.reports.at(5).watermark, 300u);
}
TEST(StoryWatermarkPublication, LockWaitConsumesTheOriginalReportDeadline)
{
    initialize();
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(5, 100);
    chl::StoryWatermarkRegistry::Contributions contributions{{5, registry.contributorToken(5)}};
    auto lock = chl::WatermarkRegistryTestAccess::lock(registry);
    int status = 0;
    auto began = std::chrono::steady_clock::now();
    std::thread waiter(
            [&]
            {
                chl::StoryWatermarkRegistry::Publication snapshot;
                status = registry.snapshotFor(contributions, chl::rpc::RpcDeadline::after(30ms), snapshot);
            });
    waiter.join();
    EXPECT_EQ(status, chl::CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_GE(std::chrono::steady_clock::now() - began, 20ms);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 150ms);
}
INSTANTIATE_TEST_SUITE_P(Native, WatermarkRpcDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
} // namespace
