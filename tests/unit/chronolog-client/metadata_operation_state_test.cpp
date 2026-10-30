#include "MetadataOperationState.h"
#include <gtest/gtest.h>
#include "RpcPolicyContext.h"
#include <future>
using namespace chronolog;
TEST(MetadataOperationState, UnconfirmedNameRejectsConflictingLifecycle)
{
    MetadataOperationState state;
    auto acquire = state.begin("chronicle", "story");
    ASSERT_TRUE(acquire);
    EXPECT_EQ(MetadataOperationState::Phase::Pending, state.phase(*acquire));
    state.finish(*acquire, false);
    EXPECT_EQ(MetadataOperationState::Phase::Unconfirmed, state.phase(*acquire));
    EXPECT_FALSE(state.begin("chronicle", "story"));
    EXPECT_FALSE(state.begin("chronicle", ""));
    EXPECT_TRUE(state.begin("other", "story"));
}
TEST(MetadataOperationState, ConfirmedLateResultOnlyResolvesMatchingGeneration)
{
    MetadataOperationState state;
    auto first = state.begin("c", "s");
    ASSERT_TRUE(first);
    state.finish(*first, true);
    auto replacement = state.begin("c", "s");
    ASSERT_TRUE(replacement);
    EXPECT_NE(first->generation, replacement->generation);
    state.finish(*first, true);
    EXPECT_FALSE(state.matches(*first));
    EXPECT_TRUE(state.matches(*replacement));
    EXPECT_EQ(MetadataOperationState::Phase::Pending, state.phase(*replacement));
    EXPECT_FALSE(state.begin("c", "s"));
    state.finish(*replacement, true);
    EXPECT_TRUE(state.begin("c", "s"));
}
TEST(MetadataOperationState, ClientLifecycleAdmissionSeesPendingNames)
{
    MetadataOperationState state;
    auto pending = state.begin("c", "s");
    ASSERT_TRUE(pending);
    EXPECT_FALSE(state.begin("", ""));
    state.finish(*pending, true);
    auto lifecycle = state.begin("", "");
    ASSERT_TRUE(lifecycle);
    EXPECT_FALSE(state.begin("c", "s"));
}

namespace chronolog
{
struct RpcPolicyContextTestAccess
{
    static std::unique_lock<std::mutex> holdPolicy(RpcPolicyContext& context)
    {
        return std::unique_lock<std::mutex>(context.state_->mutex);
    }
};
} // namespace chronolog
TEST(RpcPolicyContext, FreezeDoesNotWaitForPolicyMutexAndStillLatches)
{
    RpcPolicyContext context(RpcTimeoutPolicy{});
    auto lock = RpcPolicyContextTestAccess::holdPolicy(context);
    auto freeze = std::async(std::launch::async, [&] { context.freeze(); });
    bool ready = freeze.wait_for(std::chrono::milliseconds(30)) == std::future_status::ready;
    lock.unlock();
    freeze.get();
    EXPECT_TRUE(ready);
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, context.setPolicy(RpcTimeoutPolicy{}));
}
