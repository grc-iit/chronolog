#include <NativeRuntime.h>
#include <RpcRuntime.h>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
using namespace chronolog;
using namespace chronolog::rpc;
using namespace std::chrono_literals;
namespace
{
struct NativeGroup
{
    std::shared_ptr<NativeProcessGeneration> process = std::make_shared<NativeProcessGeneration>(3);
    std::array<NativeMidRuntime, 3> mids{NativeMidRuntime(process),
                                         NativeMidRuntime(process),
                                         NativeMidRuntime(process)};
    std::string protocol;
    std::shared_ptr<std::atomic<bool>> retired;
    NativeGroup(std::string protocol, std::shared_ptr<std::atomic<bool>> retired)
        : protocol(std::move(protocol))
        , retired(std::move(retired))
    {}
    void cleanup()
    {
        for(auto& mid: mids) mid.cleanup();
        if(!process->cleanupComplete())
            throw std::runtime_error("Process generation did not retire after the final MID");
        retired->store(true, std::memory_order_release);
    }
};
struct ConstructGroup: OwnedRpcWork<int>
{
    std::shared_ptr<NativeGroup> group;
    std::shared_ptr<std::atomic<bool>> entered, release;
    ConstructGroup(std::shared_ptr<NativeGroup> group,
                   std::shared_ptr<std::atomic<bool>> entered = {},
                   std::shared_ptr<std::atomic<bool>> release = {})
        : group(std::move(group))
        , entered(std::move(entered))
        , release(std::move(release))
    {}
    RpcResult<int> run(OperationContext&) override
    {
        if(entered)
            entered->store(true, std::memory_order_release);
        if(release)
            while(!release->load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        for(unsigned i = 0; i < group->mids.size(); ++i)
        {
            int status = group->mids[i].initialize({group->protocol + "://127.0.0.1:0", MARGO_SERVER_MODE, i + 1});
            if(status != CL_SUCCESS)
                return {status, false, status};
        }
        int streams = -1;
        if(ABT_xstream_get_num(&streams) != ABT_SUCCESS)
            throw std::runtime_error("Cannot count actual native streams");
        return {CL_SUCCESS, false, streams};
    }
};
bool until(std::function<bool()> condition)
{
    auto end = RpcDeadline::Clock::now() + 5s;
    while(!condition())
    {
        if(RpcDeadline::Clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
class NativeRuntime: public ::testing::TestWithParam<char const*>
{
};
TEST_P(NativeRuntime, ThreeActualMidsShareOneGenerationAndPreserveConfiguredHandlerStreams)
{
    ASSERT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    auto retired = std::make_shared<std::atomic<bool>>(false);
    auto group = std::make_shared<NativeGroup>(GetParam(), retired);
    std::weak_ptr<NativeGroup> weak = group;
    auto physical = group->process;
    RpcRuntime runtime;
    runtime.retainOwner(group, [group] { group->cleanup(); });
    auto result = runtime.invoke<int>(RpcDeadline::after(5s), std::make_shared<ConstructGroup>(group));
    ASSERT_EQ(CL_SUCCESS, result.status);
    ASSERT_TRUE(result.reply.has_value());
    // One original ABI primary, three native progress ES, and 1+2+3 handler ES.
    EXPECT_EQ(10, *result.reply);
    group.reset();
    EXPECT_EQ(CL_SUCCESS, runtime.drain(RpcDeadline::after(5s)));
    EXPECT_TRUE(retired->load());
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
    auto proof = physical->snapshot();
    EXPECT_EQ(3u, proof.initializedMids);
    EXPECT_EQ(3u, proof.finalizeCalls);
    EXPECT_EQ(3u, proof.finalizeReturns);
    EXPECT_EQ(3u, proof.finalizedMids);
    EXPECT_EQ(3u, proof.constructorUnitsCreated);
    EXPECT_EQ(3u, proof.constructorUnitsRetired);
    EXPECT_EQ(6u, proof.rpcStreamsCreated);
    EXPECT_EQ(6u, proof.rpcStreamsRetired);
    EXPECT_EQ(3u, proof.poolsCreated);
    EXPECT_EQ(3u, proof.poolsRetired);
    EXPECT_EQ(1u, proof.primaryReleases);
    EXPECT_EQ(1, proof.streamsBeforeRelease);
    EXPECT_TRUE(proof.samePrimary);
    EXPECT_TRUE(proof.abtUninitialized);
    EXPECT_TRUE(proof.complete);
    size_t actualStackSize = 0;
    // New cold generation must still be supported by the same process owner.
    auto next = std::make_shared<NativeGroup>(GetParam(), retired);
    RpcRuntime nextRuntime;
    nextRuntime.retainOwner(next, [next] { next->cleanup(); });
    ASSERT_EQ(CL_SUCCESS,
              nextRuntime.invoke<int>(RpcDeadline::after(5s), std::make_shared<ConstructGroup>(next)).status);
    ASSERT_EQ(ABT_SUCCESS, ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &actualStackSize));
    EXPECT_EQ(2097152u, actualStackSize);
    next.reset();
    EXPECT_EQ(CL_SUCCESS, nextRuntime.drain(RpcDeadline::after(5s)));
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
}
TEST_P(NativeRuntime, ExpiredCallerAndDroppedFacadeRetainTheOriginalConstructorAndCleanup)
{
    auto retired = std::make_shared<std::atomic<bool>>(false);
    auto entered = std::make_shared<std::atomic<bool>>(false), release = std::make_shared<std::atomic<bool>>(false);
    struct Release
    {
        std::shared_ptr<std::atomic<bool>> value;
        ~Release() { *value = true; }
    } finally{release};
    auto group = std::make_shared<NativeGroup>(GetParam(), retired);
    std::weak_ptr<NativeGroup> weak = group;
    auto runtime = std::make_unique<RpcRuntime>();
    runtime->retainOwner(group, [group] { group->cleanup(); });
    auto started = RpcDeadline::Clock::now();
    auto pending = runtime->submit<int>(runtime->reserve(RpcDeadline::fromStart(started, 40ms)),
                                        std::make_shared<ConstructGroup>(group, entered, release));
    ASSERT_TRUE(until([&] { return entered->load(); }));
    auto result = pending.wait();
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.status);
    EXPECT_FALSE(result.may_have_dispatched);
    EXPECT_LT(RpcDeadline::Clock::now() - started, 200ms);
    group.reset();
    EXPECT_EQ(CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(RpcDeadline::after(40ms)));
    runtime.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(retired->load());
    release->store(true, std::memory_order_release);
    ASSERT_TRUE(until([&] { return retired->load(std::memory_order_acquire) && weak.expired(); }));
    EXPECT_EQ(ABT_ERR_UNINITIALIZED, ABT_initialized());
}
INSTANTIATE_TEST_SUITE_P(Transports, NativeRuntime, ::testing::Values("ofi+sockets", "ofi+tcp"));
} // namespace
