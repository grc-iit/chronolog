#include <PlaybackRpcOwner.h>
#include <gtest/gtest.h>
#include <future>
#include <condition_variable>
using namespace std::chrono_literals;
namespace
{
namespace rpc = chronolog::rpc;
struct Held
{
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, release = false, destroyed = false;
    std::thread::id destructionThread;
};
class Work: public rpc::OwnedRpcWork<int>
{
    std::shared_ptr<Held> held_;

public:
    explicit Work(std::shared_ptr<Held> held)
        : held_(std::move(held))
    {}
    ~Work()
    {
        std::lock_guard<std::mutex> lock(held_->mutex);
        held_->destroyed = true;
        held_->destructionThread = std::this_thread::get_id();
        held_->changed.notify_all();
    }
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        if(!operation.tryBeginDispatch())
            return {chronolog::CL_ERR_DEADLINE_EXCEEDED, false, 0};
        std::unique_lock<std::mutex> lock(held_->mutex);
        held_->started = true;
        held_->changed.notify_all();
        held_->changed.wait(lock, [&] { return held_->release; });
        return {chronolog::CL_SUCCESS, true, 0};
    }
};
class PlayerQueries: public testing::Test
{
protected:
    std::shared_ptr<rpc::RpcRuntime> runtime = std::make_shared<rpc::RpcRuntime>(6);
    // No native MID is initialized in these gate/charged-cleanup tests.
    std::shared_ptr<chronolog::PlaybackRpcOwner> owner = chronolog::PlaybackRpcOwner::Prepare(
            runtime,
            {},
            std::make_unique<rpc::NativeMidRuntime>(std::make_shared<rpc::NativeProcessGeneration>(3)));
    void TearDown() override
    {
        owner->quiesce();
        owner->retirementReady = true;
        EXPECT_EQ(chronolog::CL_SUCCESS, runtime->drain(rpc::RpcDeadline::after(1s)));
    }
};
TEST_F(PlayerQueries, PreEnteredSubmissionMustSettleAfterTheOldIdleFirstObservation)
{
    auto entered = owner->enter();
    ASSERT_TRUE(entered);
    std::promise<void> idle;
    auto observed = idle.get_future();
    auto fence = std::async(
            std::launch::async,
            [&] { return owner->quiesceQueries(rpc::RpcDeadline::after(500ms), [&] { idle.set_value(); }); });
    auto until = std::chrono::steady_clock::now() + 100ms;
    while(!owner->closing() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    ASSERT_TRUE(owner->closing());
    // With the old order this confirms all six slots were observed before
    // the original already-entered handler makes its reservation/submission.
    EXPECT_NE(observed.wait_for(20ms), std::future_status::ready);
    auto held = std::make_shared<Held>();
    struct Release
    {
        std::shared_ptr<Held> value;
        ~Release()
        {
            std::lock_guard<std::mutex> lock(value->mutex);
            value->release = true;
            value->changed.notify_all();
        }
    } finally{held};
    auto token = runtime->reserve(rpc::RpcDeadline::after(1s));
    auto availableUntil = std::chrono::steady_clock::now() + 100ms;
    while(!token && token.status() == chronolog::CL_ERR_RPC_CAPACITY &&
          std::chrono::steady_clock::now() < availableUntil)
    {
        std::this_thread::sleep_for(1ms);
        token = runtime->reserve(rpc::RpcDeadline::after(1s));
    }
    ASSERT_TRUE(token); // Actual original observation-token cleanup, not a sleep.
    auto pending = runtime->submit<int>(std::move(token), std::make_shared<Work>(held));
    {
        std::unique_lock<std::mutex> lock(held->mutex);
        ASSERT_TRUE(held->changed.wait_for(lock, 100ms, [&] { return held->started; }));
    }
    entered.reset();
    EXPECT_EQ(fence.wait_for(20ms), std::future_status::timeout);
    EXPECT_FALSE(owner->queriesSettled.load());
    {
        std::lock_guard<std::mutex> lock(held->mutex);
        held->release = true;
        held->changed.notify_all();
    }
    EXPECT_EQ(chronolog::CL_SUCCESS, pending.wait().status);
    EXPECT_TRUE(fence.get());
    {
        std::lock_guard<std::mutex> lock(held->mutex);
        EXPECT_TRUE(held->destroyed);
        EXPECT_NE(held->destructionThread, std::this_thread::get_id());
    }
    EXPECT_TRUE(owner->queriesSettled.load());
}
TEST_F(PlayerQueries, ClosureRejectsNewEntriesAndReservationsWithoutDispatch)
{
    ASSERT_TRUE(owner->quiesceQueries(rpc::RpcDeadline::after(100ms)));
    EXPECT_FALSE(owner->enter());
    EXPECT_FALSE(owner->reserveQuery(rpc::RpcDeadline::after(100ms)));
    EXPECT_TRUE(owner->queriesSettled.load());
}
TEST_F(PlayerQueries, HeldOriginalC6CannotProveAndLaterWaitUsesItsCleanup)
{
    auto token = runtime->reserve(rpc::RpcDeadline::after(1s));
    ASSERT_TRUE(token);
    EXPECT_FALSE(owner->quiesceQueries(rpc::RpcDeadline::after(30ms)));
    EXPECT_FALSE(owner->queriesSettled.load());
    token = {};
    EXPECT_TRUE(owner->quiesceQueries(rpc::RpcDeadline::after(300ms)));
    EXPECT_TRUE(owner->queriesSettled.load());
}
} // namespace
