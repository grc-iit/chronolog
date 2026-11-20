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
using Kind = MetadataOperationState::Kind;
// After an acquire with an unknown outcome, the same story's next acquire or
// release is sent as a retry; every other change that names the story or its
// chronicle stays refused. A confirmed retry clears the record.
TEST(MetadataOperationState, UnconfirmedAcquireAdmitsARetriedAcquireOrRelease)
{
    MetadataOperationState state;
    auto acquire = state.begin("c", "s", Kind::Acquire);
    ASSERT_TRUE(acquire);
    EXPECT_FALSE(acquire->retry);
    state.finish(*acquire, false);
    EXPECT_FALSE(state.begin("c", "s"));
    EXPECT_FALSE(state.begin("c", ""));
    EXPECT_FALSE(state.begin("", ""));
    EXPECT_FALSE(state.conflicts("c", "s", Kind::Release));
    auto retry = state.begin("c", "s", Kind::Acquire);
    ASSERT_TRUE(retry);
    EXPECT_TRUE(retry->retry);
    EXPECT_FALSE(state.matches(*acquire));
    // A retry in flight blocks another one.
    EXPECT_FALSE(state.begin("c", "s", Kind::Release));
    state.finish(*retry, true);
    EXPECT_FALSE(state.unresolved());
    auto fresh = state.begin("c", "s", Kind::Release);
    ASSERT_TRUE(fresh);
    EXPECT_FALSE(fresh->retry);
}
// A retry whose own outcome is unknown can be retried again.
TEST(MetadataOperationState, UnconfirmedRetryStaysRetryable)
{
    MetadataOperationState state;
    auto acquire = state.begin("c", "s", Kind::Acquire);
    state.finish(*acquire, false);
    auto release = state.begin("c", "s", Kind::Release);
    ASSERT_TRUE(release && release->retry);
    state.finish(*release, false);
    auto again = state.begin("c", "s", Kind::Release);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->retry);
}
// A retried release that changed nothing (answered locally, or never sent)
// leaves the acquire's unknown outcome as it was, so a retried acquire is
// still admitted.
TEST(MetadataOperationState, RetryThatChangedNothingKeepsTheEarlierOutcome)
{
    MetadataOperationState state;
    auto acquire = state.begin("c", "s", Kind::Acquire);
    state.finish(*acquire, false);
    auto release = state.begin("c", "s", Kind::Release);
    ASSERT_TRUE(release && release->retry);
    state.keepEarlier(*release);
    EXPECT_TRUE(state.unresolved());
    auto again = state.begin("c", "s", Kind::Acquire);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->retry);
}
// After a release with an unknown outcome the client still has the story's
// handle, so only another release is sent.
TEST(MetadataOperationState, UnconfirmedReleaseAdmitsOnlyARetriedRelease)
{
    MetadataOperationState state;
    auto release = state.begin("c", "s", Kind::Release);
    state.finish(*release, false);
    EXPECT_FALSE(state.begin("c", "s", Kind::Acquire));
    auto retry = state.begin("c", "s", Kind::Release);
    ASSERT_TRUE(retry);
    EXPECT_TRUE(retry->retry);
}
// Disconnect is sent while only acquires and releases have unknown outcomes,
// and a confirmed one clears them. Any other unknown outcome, or a change still
// in flight, blocks it.
TEST(MetadataOperationState, DisconnectSettlesUnknownAcquiresAndReleases)
{
    MetadataOperationState state;
    auto acquire = state.begin("c", "s", Kind::Acquire);
    state.finish(*acquire, false);
    auto release = state.begin("c", "t", Kind::Release);
    state.finish(*release, false);
    EXPECT_TRUE(state.unresolved());
    EXPECT_FALSE(state.unresolvedBeyondAcquisitions());
    auto disconnect = state.begin("", "", Kind::Disconnect);
    ASSERT_TRUE(disconnect);
    EXPECT_FALSE(disconnect->retry);
    state.finish(*disconnect, true);
    state.settleAcquisitions();
    EXPECT_FALSE(state.unresolved());

    auto pending = state.begin("c", "s", Kind::Acquire);
    ASSERT_TRUE(pending);
    EXPECT_FALSE(state.begin("", "", Kind::Disconnect));
    state.finish(*pending, true);
    auto destroy = state.begin("c", "s");
    state.finish(*destroy, false);
    EXPECT_TRUE(state.unresolvedBeyondAcquisitions());
    EXPECT_FALSE(state.begin("", "", Kind::Disconnect));
    state.settleAcquisitions();
    EXPECT_TRUE(state.unresolved());
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
