#include <gtest/gtest.h>
#include <RpcRuntime.h>
#include <thallium/timeout.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace chronolog;
using namespace chronolog::rpc;

namespace
{
struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;
    void block()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return open; });
    }
    bool waitForEntry()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        changed.notify_all();
    }
};
struct ReleaseGates
{
    std::vector<std::shared_ptr<Gate>> gates;
    ~ReleaseGates()
    {
        for(auto const& gate: gates) gate->release();
    }
};

template <class Reply>
struct Work: OwnedRpcWork<Reply>
{
    std::function<RpcResult<Reply>(OperationContext&)> body;
    explicit Work(std::function<RpcResult<Reply>(OperationContext&)> f)
        : body(std::move(f))
    {}
    RpcResult<Reply> run(OperationContext& context) override { return body(context); }
};

template <class Reply = int, class Fn>
std::shared_ptr<OwnedRpcWork<Reply>> work(Fn&& fn)
{
    return std::make_shared<Work<Reply>>(std::forward<Fn>(fn));
}

RpcResult<int> confirmed(int value = 7) { return {CL_SUCCESS, true, value}; }

bool eventually(std::function<bool()> const& condition)
{
    auto until = RpcDeadline::Clock::now() + 2s;
    while(!condition())
    {
        if(RpcDeadline::Clock::now() >= until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

struct DestructionGateWork: OwnedRpcWork<int>
{
    std::shared_ptr<Gate> destruction;
    explicit DestructionGateWork(std::shared_ptr<Gate> gate)
        : destruction(std::move(gate))
    {}
    ~DestructionGateWork() override { destruction->block(); }
    RpcResult<int> run(OperationContext& context) override
    {
        if(!context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, {}};
        return confirmed();
    }
};

struct BlockingReply
{
    std::shared_ptr<Gate> publication;
    std::shared_ptr<std::atomic<bool>> firstMove;
    explicit BlockingReply(std::shared_ptr<Gate> gate)
        : publication(std::move(gate))
        , firstMove(std::make_shared<std::atomic<bool>>(true))
    {}
    BlockingReply(BlockingReply&& other) noexcept
        : publication(std::move(other.publication))
        , firstMove(std::move(other.firstMove))
    {
        if(firstMove && firstMove->exchange(false))
            publication->block();
    }
    BlockingReply(BlockingReply const&) = delete;
};
} // namespace

TEST(RpcRuntime, DeliversConfirmedRemoteRejection)
{
    RpcRuntime runtime;
    auto result = runtime.invoke<int>(RpcDeadline::after(1s),
                                      work(
                                              [](OperationContext& context)
                                              {
                                                  if(!context.tryBeginDispatch())
                                                      return RpcResult<int>{CL_ERR_DEADLINE_EXCEEDED, false, {}};
                                                  return RpcResult<int>{CL_ERR_NO_KEEPERS, true, 0};
                                              }));
    EXPECT_EQ(result.status, CL_ERR_NO_KEEPERS);
    EXPECT_TRUE(result.may_have_dispatched);
    ASSERT_TRUE(result.reply);
    EXPECT_EQ(*result.reply, 0);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, ExpiredEntryNeverRunsOrDispatches)
{
    RpcRuntime runtime;
    std::atomic<int> runs{0};
    auto deadline = RpcDeadline::fromStart(RpcDeadline::Clock::now() - 1s, 1ms);
    auto result = runtime.invoke<int>(deadline,
                                      work(
                                              [&](OperationContext&)
                                              {
                                                  ++runs;
                                                  return confirmed();
                                              }));
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_FALSE(result.reply);
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, BlockedOwnedLookupExpiresWithoutForwardingOrLosingOwners)
{
    RpcRuntime runtime;
    auto lookup = std::make_shared<Gate>();
    ReleaseGates cleanup{{lookup}};
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    auto forwards = std::make_shared<std::atomic<int>>(0);
    auto started = RpcDeadline::Clock::now();
    auto result = runtime.invoke<int>(RpcDeadline::fromStart(started, 40ms),
                                      work(
                                              [lookup, owner, forwards](OperationContext& context)
                                              {
                                                  lookup->block();
                                                  EXPECT_EQ(*owner,
                                                            42); // lookup still accesses its lease after caller expiry
                                                  if(!context.tryBeginDispatch())
                                                      return RpcResult<int>{CL_ERR_DEADLINE_EXCEEDED, false, {}};
                                                  ++*forwards;
                                                  return confirmed(*owner);
                                              }));
    owner.reset();
    EXPECT_TRUE(lookup->waitForEntry());
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_LT(RpcDeadline::Clock::now() - started, 500ms);
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(runtime.drain(RpcDeadline::after(20ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
    EXPECT_FALSE(weak.expired());
    lookup->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
    EXPECT_EQ(*forwards, 0);
    EXPECT_TRUE(weak.expired());
}

TEST(RpcRuntime, DispatchReservationWinsBeforeCallerExpiry)
{
    RpcRuntime runtime;
    auto reply = std::make_shared<Gate>();
    ReleaseGates cleanup{{reply}};
    auto result = runtime.invoke<int>(RpcDeadline::after(40ms),
                                      work(
                                              [reply](OperationContext& context)
                                              {
                                                  if(!context.tryBeginDispatch())
                                                      return RpcResult<int>{CL_ERR_DEADLINE_EXCEEDED, false, {}};
                                                  reply->block();
                                                  return confirmed();
                                              }));
    EXPECT_TRUE(reply->waitForEntry());
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_TRUE(result.may_have_dispatched);
    EXPECT_FALSE(result.reply);
    reply->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, CompletionWinsBeforeExpiry)
{
    RpcRuntime runtime;
    auto result = runtime.invoke<int>(RpcDeadline::after(1s),
                                      work(
                                              [](OperationContext& context)
                                              {
                                                  EXPECT_TRUE(context.tryBeginDispatch());
                                                  return confirmed(73);
                                              }));
    ASSERT_TRUE(result.reply);
    EXPECT_EQ(*result.reply, 73);
    EXPECT_EQ(result.status, CL_SUCCESS);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, ResultPublicationContentionDoesNotHoldCaller)
{
    RpcRuntime runtime;
    auto publication = std::make_shared<Gate>();
    ReleaseGates cleanup{{publication}};
    auto started = RpcDeadline::Clock::now();
    auto result = runtime.invoke<BlockingReply>(
            RpcDeadline::fromStart(started, 40ms),
            work<BlockingReply>(
                    [publication](OperationContext& context)
                    {
                        EXPECT_TRUE(context.tryBeginDispatch());
                        return RpcResult<BlockingReply>{CL_SUCCESS,
                                                        true,
                                                        std::optional<BlockingReply>(std::in_place, publication)};
                    }));
    EXPECT_TRUE(publication->waitForEntry());
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_FALSE(result.reply);
    EXPECT_LT(RpcDeadline::Clock::now() - started, 500ms);
    publication->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
    EXPECT_FALSE(result.reply);
}

TEST(RpcRuntime, TwoExpiredLookupsSaturateExactlyTwoSlots)
{
    RpcRuntime runtime;
    auto lookup1 = std::make_shared<Gate>();
    auto lookup2 = std::make_shared<Gate>();
    ReleaseGates cleanup{{lookup1, lookup2}};
    for(auto const& lookup: {lookup1, lookup2})
    {
        auto result = runtime.invoke<int>(RpcDeadline::after(20ms),
                                          work(
                                                  [lookup](OperationContext& context)
                                                  {
                                                      lookup->block();
                                                      if(!context.tryBeginDispatch())
                                                          return RpcResult<int>{CL_ERR_DEADLINE_EXCEEDED, false, {}};
                                                      return confirmed();
                                                  }));
        EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
        EXPECT_TRUE(lookup->waitForEntry());
    }
    std::atomic<int> unexpected{0};
    auto started = RpcDeadline::Clock::now();
    for(int i = 0; i != 32; ++i)
    {
        auto result = runtime.invoke<int>(RpcDeadline::after(1s),
                                          work(
                                                  [&](OperationContext&)
                                                  {
                                                      ++unexpected;
                                                      return confirmed();
                                                  }));
        EXPECT_EQ(result.status, CL_ERR_RPC_CAPACITY);
        EXPECT_FALSE(result.may_have_dispatched);
    }
    EXPECT_EQ(unexpected, 0);
    EXPECT_LT(RpcDeadline::Clock::now() - started, 200ms);
    lookup1->release();
    lookup2->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, ReservationChargesCapacityBeforePayloadPreparation)
{
    RpcRuntime runtime;
    auto first = runtime.reserve(RpcDeadline::after(1s));
    auto second = runtime.reserve(RpcDeadline::after(1s));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    int allocations = 0;
    for(int i = 0; i != 32; ++i)
    {
        auto rejected = runtime.reserve(RpcDeadline::after(1s));
        if(rejected)
            ++allocations;
        EXPECT_EQ(rejected.status(), CL_ERR_RPC_CAPACITY);
    }
    EXPECT_EQ(allocations, 0);
    first = {};
    second = {};
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, UnusedReservationReleasesOnlyThroughOwnedCleanup)
{
    RpcRuntime runtime;
    {
        auto reservation = runtime.reserve(RpcDeadline::after(1s));
        ASSERT_TRUE(reservation);
    }
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
    auto rejected = runtime.reserve(RpcDeadline::after(1s));
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status(), CL_ERR_RPC_CAPACITY);
}

TEST(RpcRuntime, ExpiredReservationQueuesDestructionWithoutRunningWork)
{
    RpcRuntime runtime;
    auto reservation = runtime.reserve(RpcDeadline::after(15ms));
    ASSERT_TRUE(reservation);
    auto destruction = std::make_shared<Gate>();
    ReleaseGates cleanup{{destruction}};
    auto pending = std::make_shared<DestructionGateWork>(destruction);
    std::this_thread::sleep_for(30ms);
    auto started = RpcDeadline::Clock::now();
    auto result = runtime.invoke<int>(std::move(reservation), std::move(pending));
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_LT(RpcDeadline::Clock::now() - started, 300ms);
    EXPECT_TRUE(destruction->waitForEntry());
    EXPECT_EQ(runtime.drain(RpcDeadline::after(20ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
    destruction->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, CloseRacingReservedSubmissionStillOwnsItsDestruction)
{
    RpcRuntime runtime;
    auto reservation = runtime.reserve(RpcDeadline::after(1s));
    ASSERT_TRUE(reservation);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(10ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
    auto destruction = std::make_shared<Gate>();
    ReleaseGates cleanup{{destruction}};
    auto result = runtime.invoke<int>(std::move(reservation), std::make_shared<DestructionGateWork>(destruction));
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_FALSE(result.reply);
    EXPECT_TRUE(destruction->waitForEntry());
    destruction->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, NativeOwnerDestructionStaysOffCallerAndKeepsCapacity)
{
    RpcRuntime runtime;
    auto first = std::make_shared<Gate>();
    auto second = std::make_shared<Gate>();
    ReleaseGates cleanup{{first, second}};
    for(auto const& gate: {first, second})
    {
        auto reservation = runtime.reserve(RpcDeadline::after(1s));
        ASSERT_TRUE(reservation);
        auto started = RpcDeadline::Clock::now();
        auto result = runtime.invoke<int>(std::move(reservation), std::make_shared<DestructionGateWork>(gate));
        EXPECT_EQ(result.status, CL_SUCCESS);
        EXPECT_LT(RpcDeadline::Clock::now() - started, 300ms);
        EXPECT_TRUE(gate->waitForEntry());
    }
    auto rejected = runtime.reserve(RpcDeadline::after(1s));
    EXPECT_EQ(rejected.status(), CL_ERR_RPC_CAPACITY);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(20ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
    first->release();
    second->release();
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, DroppingRuntimeHandleRetainsBlockedOwnerUntilSettlement)
{
    auto runtime = std::make_unique<RpcRuntime>();
    auto lookup = std::make_shared<Gate>();
    ReleaseGates cleanup{{lookup}};
    auto owner = std::make_shared<int>(9);
    std::weak_ptr<int> weak = owner;
    auto result = runtime->invoke<int>(RpcDeadline::after(20ms),
                                       work(
                                               [owner, lookup](OperationContext& context)
                                               {
                                                   lookup->block();
                                                   if(!context.tryBeginDispatch())
                                                       return RpcResult<int>{CL_ERR_DEADLINE_EXCEEDED, false, {}};
                                                   return confirmed(*owner);
                                               }));
    owner.reset();
    auto started = RpcDeadline::Clock::now();
    runtime.reset();
    EXPECT_LT(RpcDeadline::Clock::now() - started, 100ms);
    EXPECT_FALSE(weak.expired());
    lookup->release();
    EXPECT_TRUE(eventually([&] { return weak.expired(); }));
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
}

TEST(RpcRuntime, UnconfirmedBulkKeepsBothSlotsAndOwnersAfterClose)
{
    auto runtime = std::make_unique<RpcRuntime>();
    std::vector<std::weak_ptr<std::vector<char>>> owners;
    for(int i = 0; i != 2; ++i)
    {
        auto reservation = runtime->reserve(RpcDeadline::after(1s));
        ASSERT_TRUE(reservation);
        auto bytes = std::make_shared<std::vector<char>>(1024 * (i + 1), 'x');
        owners.push_back(bytes);
        auto result = runtime->invoke<int>(std::move(reservation),
                                           work(
                                                   [bytes](OperationContext& context)
                                                   {
                                                       EXPECT_TRUE(context.tryBeginDispatch());
                                                       return RpcResult<int>{CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
                                                   }));
        EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
        EXPECT_TRUE(result.may_have_dispatched);
    }
    EXPECT_EQ(runtime->reserve(RpcDeadline::after(1s)).status(), CL_ERR_RPC_CAPACITY);
    EXPECT_EQ(runtime->drain(RpcDeadline::after(20ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
    runtime.reset();
    for(auto const& owner: owners) EXPECT_FALSE(owner.expired());
}

TEST(RpcRuntime, CatchesThalliumTimeoutSeparatelyBeforeDispatch)
{
    RpcRuntime runtime;
    auto result = runtime.invoke<int>(RpcDeadline::after(1s),
                                      work([](OperationContext&) -> RpcResult<int> { throw thallium::timeout(); }));
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
}

TEST(RpcRuntime, CatchesThalliumTimeoutSeparatelyAfterDispatchAndRetainsOwner)
{
    RpcRuntime runtime;
    auto owner = std::make_shared<int>(11);
    std::weak_ptr<int> weak = owner;
    auto result = runtime.invoke<int>(RpcDeadline::after(1s),
                                      work(
                                              [owner](OperationContext& context) -> RpcResult<int>
                                              {
                                                  EXPECT_TRUE(context.tryBeginDispatch());
                                                  throw thallium::timeout();
                                              }));
    owner.reset();
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_TRUE(result.may_have_dispatched);
    EXPECT_FALSE(weak.expired());
    EXPECT_EQ(runtime.drain(RpcDeadline::after(20ms)), CL_ERR_SHUTDOWN_INCOMPLETE);
}

TEST(RpcRuntime, ConcurrentCompletionAndCloseDrainEveryAdmittedOwner)
{
    for(int round = 0; round != 20; ++round)
    {
        RpcRuntime runtime;
        auto gate = std::make_shared<Gate>();
        ReleaseGates cleanup{{gate}};
        auto active = std::async(std::launch::async,
                                 [&runtime, gate]
                                 {
                                     return runtime.invoke<int>(RpcDeadline::after(1s),
                                                                work(
                                                                        [gate](OperationContext& context)
                                                                        {
                                                                            EXPECT_TRUE(context.tryBeginDispatch());
                                                                            gate->block();
                                                                            return confirmed();
                                                                        }));
                                 });
        EXPECT_TRUE(gate->waitForEntry());
        auto stopping = std::async(std::launch::async, [&runtime] { return runtime.drain(RpcDeadline::after(1s)); });
        gate->release();
        EXPECT_EQ(active.get().status, CL_SUCCESS);
        EXPECT_EQ(stopping.get(), CL_SUCCESS);
    }
}

namespace
{
struct EngineLease
{
    std::vector<char> endpointState = std::vector<char>(65537, 'q');
};
struct LifecycleState
{
    std::shared_ptr<Gate> construction = std::make_shared<Gate>();
    std::shared_ptr<Gate> finalization = std::make_shared<Gate>();
    std::weak_ptr<EngineLease> engine;
    std::atomic<std::size_t> checksum{0};
};
struct LifecycleWork: OwnedRpcWork<int>
{
    std::shared_ptr<LifecycleState> state;
    std::shared_ptr<EngineLease> engine;
    explicit LifecycleWork(std::shared_ptr<LifecycleState> value)
        : state(std::move(value))
    {}
    RpcResult<int> run(OperationContext&) override
    {
        state->construction->block();
        engine = std::make_shared<EngineLease>();
        state->engine = engine;
        return {CL_SUCCESS, false, 1};
    }
    ~LifecycleWork() override
    {
        if(engine)
        {
            state->finalization->block();
            std::size_t value = 0;
            for(unsigned char byte: engine->endpointState) value += byte;
            state->checksum.store(value);
        }
    }
};
std::size_t threadCount()
{
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                                                  std::filesystem::directory_iterator{}));
}
} // namespace

TEST(RpcRuntime, BootstrapAndFinalizationRetainEngineAfterHandleExpiry)
{
    auto runtime = std::make_unique<RpcRuntime>();
    auto state = std::make_shared<LifecycleState>();
    ReleaseGates cleanup{{state->construction, state->finalization}};
    auto reservation = runtime->reserve(RpcDeadline::after(20ms));
    ASSERT_TRUE(reservation);
    auto result = runtime->invoke<int>(std::move(reservation), std::make_shared<LifecycleWork>(state));
    EXPECT_EQ(result.status, CL_ERR_DEADLINE_EXCEEDED);
    EXPECT_TRUE(state->construction->waitForEntry());
    runtime.reset();
    state->construction->release();
    EXPECT_TRUE(state->finalization->waitForEntry());
    EXPECT_FALSE(state->engine.expired());
    state->finalization->release();
    EXPECT_TRUE(eventually([&] { return state->engine.expired(); }));
    EXPECT_EQ(state->checksum, 7405681u);
}

TEST(RpcRuntime, SuccessfulDrainReclaimsJoinableWorkers)
{
    auto before = threadCount();
    for(int i = 0; i != 20; ++i)
    {
        RpcRuntime runtime;
        EXPECT_EQ(runtime.drain(RpcDeadline::after(1s)), CL_SUCCESS);
    }
    // The only persistent addition is the single process-wide reaper.
    EXPECT_TRUE(eventually([&] { return threadCount() <= before + 1; }));
}
