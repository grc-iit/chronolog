#include <LifecycleRecord.h>
#include <gtest/gtest.h>
using namespace chronolog;
using namespace chronolog::rpc;
using namespace std::chrono_literals;
TEST(LifecycleJoin, ExpiryRetainsTheOriginalJoinAndLaterWaitCompletes)
{
    auto record = std::make_shared<JoinRecord>();
    auto entered = std::make_shared<std::atomic<unsigned>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    struct Release
    {
        std::shared_ptr<std::atomic<bool>> value;
        ~Release() { *value = true; }
    } finally{release};
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weak = owner;
    RpcRuntime runtime;
    auto join = [entered, release]
    {
        ++*entered;
        while(!release->load()) std::this_thread::sleep_for(1ms);
    };
    ASSERT_EQ(CL_SUCCESS, record->start(runtime, RpcDeadline::after(40ms), owner, join));
    owner.reset();
    auto started = RpcDeadline::Clock::now();
    EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, record->wait(RpcDeadline::fromStart(started, 40ms)));
    EXPECT_LT(RpcDeadline::Clock::now() - started, 150ms);
    EXPECT_EQ(1u, entered->load());
    EXPECT_FALSE(weak.expired());
    // Repeated stop observes that original join and does not reserve another.
    ASSERT_EQ(CL_SUCCESS, record->start(runtime, RpcDeadline::after(40ms), {}, join));
    EXPECT_EQ(1u, entered->load());
    *release = true;
    EXPECT_EQ(CL_SUCCESS, record->wait(RpcDeadline::after(1s)));
    EXPECT_EQ(CL_SUCCESS, runtime.drain(RpcDeadline::after(1s)));
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(1u, entered->load());
}
TEST(LifecycleJoin, QueuedExpiryDoesNotPoisonTheLaterStop)
{
    auto record = std::make_shared<JoinRecord>();
    RpcRuntime runtime(1);
    auto held = runtime.reserve(RpcDeadline::after(1s));
    ASSERT_TRUE(held);
    auto owner = std::make_shared<int>(42);
    unsigned joins = 0;
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, record->start(runtime, RpcDeadline::after(10ms), owner, [&] { ++joins; }));
    held = {};
    auto until = RpcDeadline::Clock::now() + 1s;
    int status = CL_ERR_RPC_CAPACITY;
    while(status == CL_ERR_RPC_CAPACITY && RpcDeadline::Clock::now() < until)
    {
        status = record->start(runtime, RpcDeadline::after(100ms), owner, [&] { ++joins; });
        if(status == CL_ERR_RPC_CAPACITY)
            std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(CL_SUCCESS, status);
    EXPECT_EQ(CL_SUCCESS, record->wait(RpcDeadline::after(1s)));
    EXPECT_EQ(1u, joins);
    EXPECT_EQ(CL_SUCCESS, runtime.drain(RpcDeadline::after(1s)));
}
