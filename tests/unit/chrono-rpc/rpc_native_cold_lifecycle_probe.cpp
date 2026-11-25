#include <RpcRuntime.h>
#include <gtest/gtest.h>
#include <margo.h>
#include <margo-monitoring.h>
#include <thallium.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>

namespace
{
using namespace chronolog;
using namespace chronolog::rpc;
using namespace std::chrono_literals;
using Clock = RpcDeadline::Clock;
void checked(int status, char const* operation)
{
    if(status != ABT_SUCCESS)
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(status));
}
bool eventually(std::function<bool()> condition)
{
    auto end = Clock::now() + 5s;
    while(!condition())
    {
        if(Clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
size_t threads()
{
    return static_cast<size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                                             std::filesystem::directory_iterator{}));
}
int streams()
{
    int count = -1;
    checked(ABT_xstream_get_num(&count), "ABT_xstream_get_num");
    return count;
}
struct Gate
{
    std::atomic<bool> entered{false};
    std::atomic<bool> open{false};
    void wait()
    {
        entered = true;
        while(!open.load()) std::this_thread::sleep_for(1ms);
    }
};
// Test-only global lifecycle. The original primary ULT never leaves this
// pthread until every native context, constructor ULT, pool and ES has retired.
class Lifecycle: public std::enable_shared_from_this<Lifecycle>
{
public:
    std::atomic<unsigned> contexts{0};
    std::atomic<bool> ready{false}, done{false};
    std::atomic<int> initStatus{-1}, finalizeStatus{-1}, streamsAtFinalize{-1};
    std::atomic<bool> primaryAtFinalize{false};
    std::thread::id initializer, finalizer;
    void start()
    {
        std::call_once(started_,
                       [&]
                       {
                           auto lease = std::make_unique<std::shared_ptr<Lifecycle>>(shared_from_this());
                           int status = pthread_create(&thread_, nullptr, &Lifecycle::entry, lease.get());
                           if(status)
                               throw std::runtime_error("pthread_create lifecycle failed");
                           lease.release();
                           created_ = true;
                       });
        while(!ready.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        checked(initStatus.load(), "ABT_init");
    }
    bool stopAndJoin()
    {
        if(contexts.load() != 0 || !created_)
            return false;
        stop_.store(true, std::memory_order_release);
        if(!eventually([&] { return done.load(std::memory_order_acquire); }))
            return false;
        return eventually([&] { return pthread_tryjoin_np(thread_, nullptr) == 0; });
    }

private:
    static void* entry(void* argument) noexcept
    {
        std::unique_ptr<std::shared_ptr<Lifecycle>> lease(static_cast<std::shared_ptr<Lifecycle>*>(argument));
        auto owner = *lease;
        lease.reset();
        owner->initializer = std::this_thread::get_id();
        // Fresh-process cold case only. Never invoke the known corrupting warm
        // Margo constructor path as a feature query.
        if(ABT_initialized() != ABT_ERR_UNINITIALIZED)
        {
            owner->initStatus = ABT_ERR_OTHER;
            owner->ready = true;
            owner->done = true;
            return nullptr;
        }
        if(margo_set_environment(nullptr) != 0)
            owner->initStatus = ABT_ERR_OTHER;
        else
            owner->initStatus = ABT_init(0, nullptr);
        owner->ready.store(true, std::memory_order_release);
        while(!owner->stop_.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        ABT_bool primary = ABT_FALSE;
        int count = -1;
        ABT_self_is_primary(&primary);
        ABT_xstream_get_num(&count);
        owner->streamsAtFinalize = count;
        owner->primaryAtFinalize = primary == ABT_TRUE;
        owner->finalizer = std::this_thread::get_id();
        if(owner->contexts.load() == 0 && count == 1 && primary == ABT_TRUE)
            owner->finalizeStatus = ABT_finalize();
        owner->done.store(true, std::memory_order_release);
        return nullptr;
    }
    std::once_flag started_;
    pthread_t thread_{};
    bool created_ = false;
    std::atomic<bool> stop_{false};
};
struct Observation
{
    std::thread::id constructorWorker, constructorULT, cleanupWorker, handler;
    std::atomic<bool> initialized{false}, retired{false}, constructorULTFreed{false};
    std::atomic<bool> distinctProgressPool{false}, handlerOnRpcES{false};
    std::atomic<unsigned> initCalls{0}, finalizeCalls{0}, rpcCalls{0};
    std::atomic<int> nativeError{0};
    std::shared_ptr<Gate> constructorGate, cleanupGate;
    std::shared_ptr<Gate> beforeEntryGate, responseGate, postWrapperGate;
    std::atomic<bool> nativeQueued{false}, providerCleared{false};
    std::thread::id providerCleanupWorker;
};
struct NativeContext
{
    std::shared_ptr<Lifecycle> life;
    std::shared_ptr<Observation> observation;
    std::string protocol;
    ABT_pool rpcPool = ABT_POOL_NULL;
    ABT_xstream rpcES = ABT_XSTREAM_NULL;
    ABT_thread constructor = ABT_THREAD_NULL;
    margo_instance_id mid = MARGO_INSTANCE_NULL;
    ABT_thread blocker = ABT_THREAD_NULL;
    margo_monitor monitor{};
    std::unique_ptr<thallium::engine> engine;
    thallium::remote_procedure ping;
    std::string address;
    explicit NativeContext(std::shared_ptr<Lifecycle> lifecycle,
                           std::string transport,
                           std::shared_ptr<Observation> state)
        : life(std::move(lifecycle))
        , observation(std::move(state))
        , protocol(std::move(transport))
    {
        ++life->contexts;
    }
    static void construct(void* argument) noexcept
    {
        auto& self = *static_cast<NativeContext*>(argument);
        auto& observed = *self.observation;
        observed.constructorULT = std::this_thread::get_id();
        if(observed.constructorGate)
            observed.constructorGate->wait();
        try
        {
            margo_init_info options = MARGO_INIT_INFO_INITIALIZER;
            options.json_config = R"({"use_progress_thread":true,"rpc_thread_count":1})";
            options.rpc_pool = self.rpcPool;
            self.monitor.uargs = self.observation.get();
            self.monitor.initialize = [](margo_instance_id, void* arg, json_object*) { return arg; };
            self.monitor.on_rpc_handler =
                    [](void* arg, double, margo_monitor_event_t event, margo_monitor_rpc_handler_args_t args)
            {
                if(event == MARGO_MONITOR_FN_END && args->ret == HG_SUCCESS)
                    static_cast<Observation*>(arg)->nativeQueued = true;
            };
            self.monitor.on_respond = [](void* arg, double, margo_monitor_event_t event, margo_monitor_respond_args_t)
            {
                auto& observed = *static_cast<Observation*>(arg);
                if(event == MARGO_MONITOR_FN_START && observed.responseGate)
                    observed.responseGate->wait();
            };
            self.monitor.on_rpc_ult = [](void* arg, double, margo_monitor_event_t event, margo_monitor_rpc_ult_args_t)
            {
                auto& observed = *static_cast<Observation*>(arg);
                if(event == MARGO_MONITOR_FN_END && observed.postWrapperGate)
                    observed.postWrapperGate->wait();
            };
            options.monitor = &self.monitor;
            ++observed.initCalls;
            self.mid = margo_init_ext((self.protocol + "://127.0.0.1:0").c_str(), MARGO_SERVER_MODE, &options);
            if(self.mid == MARGO_INSTANCE_NULL)
                throw std::runtime_error("margo_init_ext failed");
            // This wrapper is explicitly non-owning (pinned engine.hpp230).
            // Only cleanup below calls native finalize_and_wait, exactly once.
            self.engine = std::make_unique<thallium::engine>(self.mid);
            self.address = static_cast<std::string>(self.engine->self());
            ABT_pool progress = ABT_POOL_NULL;
            checked(margo_get_progress_pool(self.mid, &progress), "margo_get_progress_pool");
            observed.distinctProgressPool = progress != self.rpcPool;
            self.ping = self.engine->define("cold_lifecycle_probe_ping",
                                            [&self](thallium::request const& request, int value)
                                            {
                                                ABT_xstream current = ABT_XSTREAM_NULL;
                                                ABT_xstream_self(&current);
                                                self.observation->handler = std::this_thread::get_id();
                                                self.observation->handlerOnRpcES = current == self.rpcES;
                                                ++self.observation->rpcCalls;
                                                request.respond(value + 1);
                                            });
            observed.initialized.store(true, std::memory_order_release);
        }
        catch(...)
        {
            observed.nativeError = 1;
        }
    }
    void initialize()
    {
        life->start();
        observation->constructorWorker = std::this_thread::get_id();
        checked(ABT_pool_create_basic(ABT_POOL_FIFO_WAIT, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &rpcPool),
                "ABT_pool_create_basic");
        checked(ABT_xstream_create_basic(ABT_SCHED_BASIC_WAIT, 1, &rpcPool, ABT_SCHED_CONFIG_NULL, &rpcES),
                "ABT_xstream_create_basic");
        checked(ABT_thread_create(rpcPool, &NativeContext::construct, this, ABT_THREAD_ATTR_NULL, &constructor),
                "ABT_thread_create");
        checked(ABT_thread_join(constructor), "ABT_thread_join constructor");
        checked(ABT_thread_free(&constructor), "ABT_thread_free constructor");
        observation->constructorULTFreed = true;
        if(!observation->initialized.load(std::memory_order_acquire))
            throw std::runtime_error("native constructor did not initialize");
    }
    void cleanup()
    {
        observation->cleanupWorker = std::this_thread::get_id();
        if(observation->cleanupGate)
            observation->cleanupGate->wait();
        if(!observation->initialized.load(std::memory_order_acquire) || !observation->constructorULTFreed.load())
            throw std::runtime_error("native initialization not proven settled");
        ++observation->finalizeCalls;
        // As NativeMidRuntime does: Margo 0.24 cleans up only when the last
        // reference on the instance goes, and runs finalize callbacks on
        // whichever thread finishes the last pending handler. Keep one
        // reference across finalization, release the procedure and the
        // wrapper here on the finalizing worker, then let Margo clean up.
        margo_instance_ref_incr(mid);
        margo_finalize_and_wait(mid);
        ping.deregister();
        ping = {};
        observation->providerCleanupWorker = std::this_thread::get_id();
        observation->providerCleared.store(true, std::memory_order_release);
        if(blocker != ABT_THREAD_NULL)
        {
            checked(ABT_thread_join(blocker), "ABT_thread_join blocker");
            checked(ABT_thread_free(&blocker), "ABT_thread_free blocker");
        }
        engine.reset(); // non-owning: no duplicate native finalize
        margo_instance_release(mid);
        mid = MARGO_INSTANCE_NULL;
        checked(ABT_xstream_join(rpcES), "ABT_xstream_join RPC");
        checked(ABT_xstream_free(&rpcES), "ABT_xstream_free RPC");
        checked(ABT_pool_free(&rpcPool), "ABT_pool_free RPC");
        observation->retired.store(true, std::memory_order_release);
        --life->contexts;
    }
};
class ConstructorWork final: public OwnedRpcWork<int>
{
public:
    explicit ConstructorWork(std::shared_ptr<NativeContext> owner)
        : owner_(std::move(owner))
    {}
    RpcResult<int> run(OperationContext&) override
    {
        owner_->initialize();
        return {CL_SUCCESS, false, 1};
    }

private:
    std::shared_ptr<NativeContext> owner_;
};
class PingWork final: public OwnedRpcWork<int>
{
public:
    PingWork(std::shared_ptr<NativeContext> from, std::shared_ptr<NativeContext> to)
        : from_(std::move(from))
        , to_(std::move(to))
    {}
    RpcResult<int> run(OperationContext& operation) override
    {
        auto endpoint = from_->engine->lookup(to_->address);
        auto attempt = operation.deadline().cappedAttempt(5s);
        if(!attempt || !operation.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, {}};
        int value = from_->ping.on(endpoint).timed(*attempt, 40);
        return {CL_SUCCESS, true, value};
    }

private:
    std::shared_ptr<NativeContext> from_, to_;
};
struct Owner
{
    std::shared_ptr<Observation> observation = std::make_shared<Observation>();
    std::shared_ptr<NativeContext> native;
    std::unique_ptr<RpcRuntime> runtime = std::make_unique<RpcRuntime>();
    Owner(std::shared_ptr<Lifecycle> life, std::string protocol)
    {
        native = std::make_shared<NativeContext>(std::move(life), std::move(protocol), observation);
        runtime->retainOwner(native, [owner = native] { owner->cleanup(); });
    }
    RpcResult<int> initialize()
    {
        auto token = runtime->reserve(RpcDeadline::after(5s));
        return runtime->invoke<int>(std::move(token), std::make_shared<ConstructorWork>(native));
    }
    bool retire()
    {
        std::weak_ptr<NativeContext> weak = native;
        native.reset();
        if(runtime->drain(RpcDeadline::after(5s)) != CL_SUCCESS)
            return false;
        runtime.reset();
        return weak.expired() && observation->retired.load() && observation->finalizeCalls.load() == 1;
    }
};
class ColdLifecycleProbe: public ::testing::TestWithParam<char const*>
{
protected:
    void order(bool reverse)
    {
        ASSERT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
        auto caller = std::this_thread::get_id();
        auto initialThreads = threads();
        auto life = std::make_shared<Lifecycle>();
        Owner a(life, GetParam()), b(life, GetParam());
        ASSERT_EQ(CL_SUCCESS, a.initialize().status);
        ASSERT_EQ(CL_SUCCESS, b.initialize().status);
        EXPECT_EQ(5, streams());
        auto liveThreads = threads();
        auto ab = a.runtime->invoke<int>(RpcDeadline::after(5s), std::make_shared<PingWork>(a.native, b.native));
        auto ba = b.runtime->invoke<int>(RpcDeadline::after(5s), std::make_shared<PingWork>(b.native, a.native));
        ASSERT_TRUE(ab.reply);
        ASSERT_TRUE(ba.reply);
        EXPECT_EQ(41, *ab.reply);
        EXPECT_EQ(41, *ba.reply);
        for(auto const& observation: {a.observation, b.observation})
        {
            EXPECT_TRUE(observation->distinctProgressPool.load());
            EXPECT_TRUE(observation->handlerOnRpcES.load());
            EXPECT_NE(caller, observation->constructorULT);
            EXPECT_NE(caller, observation->constructorWorker);
            EXPECT_NE(observation->constructorWorker, observation->constructorULT);
            EXPECT_EQ(observation->constructorULT, observation->handler);
            EXPECT_TRUE(observation->constructorULTFreed.load());
            EXPECT_EQ(1u, observation->rpcCalls.load());
        }
        EXPECT_EQ(liveThreads, threads());
        EXPECT_TRUE(reverse ? b.retire() : a.retire());
        EXPECT_EQ(3, streams());
        EXPECT_EQ(1u, life->contexts.load());
        EXPECT_TRUE(reverse ? a.retire() : b.retire());
        EXPECT_EQ(1, streams());
        EXPECT_EQ(0u, life->contexts.load());
        EXPECT_TRUE(life->stopAndJoin());
        EXPECT_EQ(ABT_SUCCESS, life->finalizeStatus.load());
        EXPECT_TRUE(life->primaryAtFinalize.load());
        EXPECT_EQ(1, life->streamsAtFinalize.load());
        EXPECT_EQ(life->initializer, life->finalizer);
        EXPECT_NE(caller, life->initializer);
        EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
        EXPECT_NE(caller, a.observation->cleanupWorker);
        EXPECT_NE(caller, b.observation->cleanupWorker);
        EXPECT_TRUE(eventually([&] { return threads() == initialThreads + 1; }));
        std::cout << "thread identities caller=" << caller << " lifecycle_init=" << life->initializer
                  << " lifecycle_finalize=" << life->finalizer << " A_worker=" << a.observation->constructorWorker
                  << " A_constructor=" << a.observation->constructorULT << " A_handler=" << a.observation->handler
                  << " A_cleanup=" << a.observation->cleanupWorker << " B_worker=" << b.observation->constructorWorker
                  << " B_constructor=" << b.observation->constructorULT << " B_handler=" << b.observation->handler
                  << " B_cleanup=" << b.observation->cleanupWorker << "\n";
        std::cout << "cold probe transport=" << GetParam() << " reverse=" << reverse
                  << " threads baseline=" << initialThreads << " two_engines=" << liveThreads << " final=" << threads()
                  << " application_workers=4 rpc_ES=2 progress_ES=2 lifecycle=1 reaper=1\n";
    }
};
TEST_P(ColdLifecycleProbe, TwoEnginesRetireAThenB) { order(false); }
TEST_P(ColdLifecycleProbe, TwoEnginesRetireBThenA) { order(true); }

TEST_P(ColdLifecycleProbe, SimulatedBlockedConstructorKeepsOriginalDeadlineAndOwner)
{
    ASSERT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    auto caller = std::this_thread::get_id();
    auto initialThreads = threads();
    auto life = std::make_shared<Lifecycle>();
    Owner owner(life, GetParam());
    auto observation = owner.observation;
    auto gate = std::make_shared<Gate>();
    struct OpenOnExit
    {
        std::shared_ptr<Gate> gate;
        ~OpenOnExit() { gate->open = true; }
    } release{gate};
    observation->constructorGate = gate;
    std::weak_ptr<NativeContext> weak = owner.native;
    auto started = Clock::now();
    auto pending = owner.runtime->submit<int>(owner.runtime->reserve(RpcDeadline::fromStart(started, 40ms)),
                                              std::make_shared<ConstructorWork>(owner.native));
    owner.native.reset();
    auto result = pending.wait();
    auto callerElapsed = Clock::now() - started;
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.status);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_LT(callerElapsed, 200ms);
    ASSERT_TRUE(eventually([&] { return gate->entered.load(); }));
    EXPECT_EQ(0u, observation->initCalls.load());
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(2, streams()); // primary + owner RPC ES, before actual Margo init
    auto held = owner.runtime->reserve(RpcDeadline::after(5s));
    ASSERT_TRUE(held);
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, owner.runtime->reserve(RpcDeadline::after(5s)).status());
    held = {};
    auto blockedThreads = threads();
    auto drainStart = Clock::now();
    EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, owner.runtime->drain(RpcDeadline::fromStart(drainStart, 40ms)));
    EXPECT_LT(Clock::now() - drainStart, 200ms);
    auto dropStart = Clock::now();
    owner.runtime.reset();
    EXPECT_LT(Clock::now() - dropStart, 200ms);
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(observation->retired.load());
    EXPECT_EQ(blockedThreads, threads());
    gate->open = true;
    ASSERT_TRUE(eventually([&] { return observation->retired.load() && weak.expired(); }));
    EXPECT_EQ(1u, observation->initCalls.load());
    EXPECT_EQ(1u, observation->finalizeCalls.load());
    EXPECT_TRUE(observation->constructorULTFreed.load());
    EXPECT_NE(caller, observation->constructorULT);
    EXPECT_NE(caller, observation->cleanupWorker);
    EXPECT_TRUE(life->stopAndJoin());
    EXPECT_EQ(ABT_SUCCESS, life->finalizeStatus.load());
    EXPECT_EQ(life->initializer, life->finalizer);
    EXPECT_TRUE(life->primaryAtFinalize.load());
    EXPECT_EQ(1, life->streamsAtFinalize.load());
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    EXPECT_TRUE(eventually([&] { return threads() == initialThreads + 1; }));
    std::cout << "thread identities caller=" << caller << " lifecycle_init=" << life->initializer
              << " lifecycle_finalize=" << life->finalizer << " worker=" << observation->constructorWorker
              << " constructor=" << observation->constructorULT << " cleanup=" << observation->cleanupWorker << "\n";
    std::cout << "simulated constructor gate BEFORE native init transport=" << GetParam()
              << " caller_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(callerElapsed).count()
              << " threads_blocked=" << blockedThreads << " final=" << threads()
              << " application_workers=2 no_RPC_dispatch=1 native_init_after_gate=1 full_retirement=1\n";
}
TEST_P(ColdLifecycleProbe, SimulatedBlockedCleanupKeepsCountAndOtherOwnerReaping)
{
    ASSERT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    auto caller = std::this_thread::get_id();
    auto initialThreads = threads();
    auto life = std::make_shared<Lifecycle>();
    Owner owner(life, GetParam());
    ASSERT_EQ(CL_SUCCESS, owner.initialize().status);
    auto observation = owner.observation;
    auto gate = std::make_shared<Gate>();
    struct OpenOnExit
    {
        std::shared_ptr<Gate> gate;
        ~OpenOnExit() { gate->open = true; }
    } release{gate};
    observation->cleanupGate = gate;
    std::weak_ptr<NativeContext> weak = owner.native;
    owner.native.reset();
    auto started = Clock::now();
    EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, owner.runtime->drain(RpcDeadline::fromStart(started, 40ms)));
    auto callerElapsed = Clock::now() - started;
    EXPECT_LT(callerElapsed, 200ms);
    ASSERT_TRUE(eventually([&] { return gate->entered.load(); }));
    EXPECT_EQ(0u, observation->finalizeCalls.load());
    EXPECT_EQ(3, streams());
    EXPECT_FALSE(weak.expired());
    auto blockedThreads = threads();
    for(unsigned i = 0; i < 8; ++i)
    {
        auto retryStarted = Clock::now();
        EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, owner.runtime->drain(RpcDeadline::fromStart(retryStarted, 3ms)));
        EXPECT_LT(Clock::now() - retryStarted, 100ms);
        EXPECT_EQ(CL_ERR_RPC_CAPACITY, owner.runtime->reserve(RpcDeadline::after(5s)).status());
        EXPECT_EQ(blockedThreads, threads());
    }
    // A blocked native finalizer must not occupy the process reaper.
    RpcRuntime independent;
    EXPECT_EQ(CL_SUCCESS, independent.drain(RpcDeadline::after(1s)));
    EXPECT_TRUE(eventually([&] { return threads() == blockedThreads; }));
    auto dropStarted = Clock::now();
    owner.runtime.reset();
    EXPECT_LT(Clock::now() - dropStarted, 200ms);
    EXPECT_FALSE(weak.expired());
    gate->open = true;
    ASSERT_TRUE(eventually([&] { return observation->retired.load() && weak.expired(); }));
    EXPECT_EQ(1u, observation->initCalls.load());
    EXPECT_EQ(1u, observation->finalizeCalls.load());
    EXPECT_NE(caller, observation->cleanupWorker);
    EXPECT_TRUE(life->stopAndJoin());
    EXPECT_EQ(ABT_SUCCESS, life->finalizeStatus.load());
    EXPECT_EQ(life->initializer, life->finalizer);
    EXPECT_TRUE(life->primaryAtFinalize.load());
    EXPECT_EQ(1, life->streamsAtFinalize.load());
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    EXPECT_TRUE(eventually([&] { return threads() == initialThreads + 1; }));
    std::cout << "thread identities caller=" << caller << " lifecycle_init=" << life->initializer
              << " lifecycle_finalize=" << life->finalizer << " worker=" << observation->constructorWorker
              << " constructor=" << observation->constructorULT << " cleanup=" << observation->cleanupWorker << "\n";
    std::cout << "simulated cleanup gate BEFORE native finalize transport=" << GetParam()
              << " caller_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(callerElapsed).count()
              << " threads_blocked=" << blockedThreads << " final=" << threads()
              << " application_workers=2 retries=8 no_growth=1 full_retirement=1\n";
}

// This child owns only the source context and a copied route. A barrier or a
// caller reference must not keep the receiver alive during its retirement.
class AddressPingWork final: public OwnedRpcWork<int>
{
    std::shared_ptr<NativeContext> from_;
    std::string address_;

public:
    AddressPingWork(std::shared_ptr<NativeContext> from, std::string address)
        : from_(std::move(from))
        , address_(std::move(address))
    {}
    RpcResult<int> run(OperationContext& operation) override
    {
        auto endpoint = from_->engine->lookup(address_);
        auto attempt = operation.deadline().cappedAttempt(5s);
        if(!attempt || !operation.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, {}};
        int result = from_->ping.on(endpoint).timed(*attempt, 40);
        return {CL_SUCCESS, true, result};
    }
};
TEST_P(ColdLifecycleProbe, OwnedFinalizerFencesQueuedResponseAndPostWrapperNativeWork)
{
    ASSERT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    auto life = std::make_shared<Lifecycle>();
    for(unsigned phase = 0; phase != 3; ++phase)
    {
        Owner source(life, GetParam()), receiver(life, GetParam());
        ASSERT_EQ(CL_SUCCESS, source.initialize().status);
        ASSERT_EQ(CL_SUCCESS, receiver.initialize().status);
        auto observed = receiver.observation;
        auto gate = std::make_shared<Gate>();
        struct OpenOnExit
        {
            std::shared_ptr<Gate> gate;
            ~OpenOnExit() { gate->open = true; }
        } release{gate};
        if(phase == 0)
        {
            observed->beforeEntryGate = gate;
            checked(ABT_thread_create(
                            receiver.native->rpcPool,
                            [](void* arg) { static_cast<Gate*>(arg)->wait(); },
                            gate.get(),
                            ABT_THREAD_ATTR_NULL,
                            &receiver.native->blocker),
                    "ABT_thread_create queued-entry blocker");
            ASSERT_TRUE(eventually([&] { return gate->entered.load(); }));
        }
        else if(phase == 1)
            observed->responseGate = gate;
        else
            observed->postWrapperGate = gate;
        auto reply =
                source.runtime->submit<int>(source.runtime->reserve(RpcDeadline::after(5s)),
                                            std::make_shared<AddressPingWork>(source.native, receiver.native->address));
        ASSERT_TRUE(eventually([&] { return observed->nativeQueued.load(); }));
        if(phase != 0)
        {
            ASSERT_TRUE(eventually([&] { return gate->entered.load(); }));
        }
        std::weak_ptr<NativeContext> weak = receiver.native;
        receiver.native.reset();
        auto started = Clock::now();
        EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, receiver.runtime->drain(RpcDeadline::fromStart(started, 40ms)));
        EXPECT_LT(Clock::now() - started, 200ms);
        ASSERT_TRUE(eventually([&] { return observed->finalizeCalls.load() == 1; }));
        EXPECT_FALSE(weak.expired());
        EXPECT_FALSE(observed->providerCleared.load());
        EXPECT_FALSE(observed->retired.load());
        if(phase == 0)
        {
            EXPECT_EQ(0u, observed->rpcCalls.load());
        }
        // Even the caller's runtime facade is gone while native finalize is
        // held. Only its accepted process-lifetime owner retains the receiver.
        receiver.runtime.reset();
        gate->open = true;
        auto result = reply.wait();
        ASSERT_TRUE(result.reply.has_value());
        EXPECT_EQ(41, *result.reply);
        ASSERT_TRUE(eventually([&] { return observed->retired.load() && weak.expired(); }));
        EXPECT_TRUE(weak.expired());
        EXPECT_TRUE(observed->providerCleared.load());
        EXPECT_EQ(observed->cleanupWorker, observed->providerCleanupWorker);
        EXPECT_EQ(1u, observed->finalizeCalls.load());
        receiver.runtime.reset();
        EXPECT_TRUE(source.retire());
        std::cout << "incoming fence transport=" << GetParam() << " phase=" << phase
                  << " caller_bound_ms=40 provider_destroyed_on_finalizer=1 native_wrapper_fenced=1\n";
    }
    ASSERT_TRUE(life->stopAndJoin());
    EXPECT_EQ(ABT_SUCCESS, life->finalizeStatus.load());
    EXPECT_EQ(life->initializer, life->finalizer);
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
}

INSTANTIATE_TEST_SUITE_P(Native, ColdLifecycleProbe, ::testing::Values("ofi+sockets", "ofi+tcp"));
} // namespace
