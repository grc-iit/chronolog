// All native peers here use loopback. For OFI TCP select its local interface
// explicitly when provider defaults choose another NIC: FI_TCP_IFACE=lo.
#include "ClientNativePeer.h"
#include "ChronologClientImpl.h"
#include <RpcDeadline.h>
#include <gtest/gtest.h>
#include <atomic>
using namespace chronolog;
using namespace std::chrono_literals;
namespace
{
using Clock = rpc::RpcDeadline::Clock;
std::atomic<Clock::duration::rep> tick{0};
Clock::time_point fakeNow() noexcept { return Clock::time_point(Clock::duration(tick.load())); }
} // namespace
namespace chronolog::rpc::detail
{
struct RpcDeadlineTestAccess
{
    static RpcDeadline fake()
    {
        auto d = RpcDeadline::fromStart(Clock::time_point{}, 5000ms);
        d.now_ = fakeNow;
        return d;
    }
};
} // namespace chronolog::rpc::detail
TEST(TailPhaseBudget, FourSecondsInSequencesLeavesOneSecondForPayloads)
{
    tick = 0;
    auto deadline = rpc::detail::RpcDeadlineTestAccess::fake();
    EXPECT_EQ(deadline.cappedAttempt(5000ms), 5000ms);
    tick = std::chrono::duration_cast<Clock::duration>(4000ms).count();
    EXPECT_EQ(deadline.cappedAttempt(5000ms), 1000ms);
    tick = std::chrono::duration_cast<Clock::duration>(4000ms + 500us).count();
    EXPECT_EQ(deadline.cappedAttempt(5000ms), 999ms);
    tick = std::chrono::duration_cast<Clock::duration>(4999ms + 500us).count();
    EXPECT_FALSE(deadline.cappedAttempt(5000ms));
    tick = std::chrono::duration_cast<Clock::duration>(5000ms).count();
    EXPECT_FALSE(deadline.cappedAttempt(5000ms));
}
int playbackWhenAdmitted(StoryHandle* handle,
                         size_t n,
                         std::vector<Event>& events,
                         test::PeerState& peer,
                         Clock::duration* elapsed = nullptr)
{
    int result = CL_ERR_RPC_CAPACITY;
    EXPECT_TRUE(test::until(
            [&]
            {
                auto sequences = peer.sequenceCalls.load(), payloads = peer.payloadCalls.load();
                auto started = Clock::now();
                result = handle->playback(n, events);
                auto duration = Clock::now() - started;
                if(result == CL_ERR_RPC_CAPACITY)
                {
                    EXPECT_EQ(sequences, peer.sequenceCalls.load());
                    EXPECT_EQ(payloads, peer.payloadCalls.load());
                    return false;
                }
                if(elapsed)
                    *elapsed = duration;
                return true;
            }));
    return result;
}
class TailDeadline: public ::testing::TestWithParam<char const*>
{
};
TEST_P(TailDeadline, BothPhasesConsumeOnePublicBudget)
{
    test::NativePeer peer(GetParam(), true);
    peer.state().keeperCount = 2;
    peer.portal.RPC_POLICY.tail_ms = 160;
    peer.state().sequenceDelayMs = 110;
    peer.state().payloadDelayMs = 110;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto handle = owner->AcquireStory("tail", "a");
    ASSERT_EQ(CL_SUCCESS, handle.first);
    std::vector<Event> events;
    Clock::duration elapsed{};
    int status = playbackWhenAdmitted(handle.second, 2, events, peer.state(), &elapsed);
    EXPECT_LT(elapsed, 210ms);
    EXPECT_EQ(CL_ERR_QUERY_TIMED_OUT, status);
    EXPECT_EQ(2u, peer.state().sequenceCalls.load());
    EXPECT_EQ(2u, peer.state().payloadCalls.load());
    EXPECT_TRUE(events.empty());
    auto context = std::weak_ptr<ClientRpcContext>(owner->context());
    owner.reset();
    EXPECT_FALSE(context.expired());
    events.emplace_back(999, 7, 3, "caller-owned sentinel");
    ASSERT_TRUE(peer.until([&] { return peer.state().payloadReplies.load() == 2; }));
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("caller-owned sentinel", events.front().log_record());
}
TEST_P(TailDeadline, RejectsRosterBeyondFixedCapacityBeforeDispatch)
{
    test::NativePeer peer(GetParam(), true);
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto handle = owner->AcquireStory("tail", "a");
    ASSERT_EQ(CL_SUCCESS, handle.first);
    std::vector<Event> events;
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, handle.second->playback(3, events));
    EXPECT_EQ(0u, peer.state().sequenceCalls.load());
    EXPECT_EQ(0u, peer.state().payloadCalls.load());
}
TEST_P(TailDeadline, TruncatedPayloadIsPartialAndNotAnEmptyCompleteRead)
{
    test::NativePeer peer(GetParam(), true);
    peer.state().keeperCount = 2;
    peer.state().shortPayloads = true;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto handle = owner->AcquireStory("tail", "a");
    ASSERT_EQ(CL_SUCCESS, handle.first);
    std::vector<Event> events;
    EXPECT_EQ(CL_ERR_PARTIAL_RESULT, playbackWhenAdmitted(handle.second, 2, events, peer.state()));
    EXPECT_TRUE(events.empty());
}
TEST_P(TailDeadline, FailedSourceReturnsUsablePartialAndKeepsItsUnknownSlotCharged)
{
    test::NativePeer peer(GetParam(), true);
    peer.state().keeperCount = 2;
    peer.state().sequenceDelayMs = 100;
    peer.state().delaySequenceMask = 1;
    peer.portal.RPC_POLICY.tail_ms = 300;
    peer.portal.RPC_POLICY.attempt_ms = 40;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto handle = owner->AcquireStory("tail", "a");
    ASSERT_EQ(CL_SUCCESS, handle.first);
    std::vector<Event> events;
    EXPECT_EQ(CL_ERR_PARTIAL_RESULT, playbackWhenAdmitted(handle.second, 2, events, peer.state()));
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(2u, peer.state().sequenceCalls.load());
    EXPECT_EQ(1u, peer.state().payloadCalls.load());
    auto original = events.front().sequence();
    ASSERT_TRUE(peer.until([&] { return peer.state().sequenceReplies.load() == 2; }));
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(original, events.front().sequence());
    EXPECT_EQ("tail", events.front().log_record());
    EXPECT_EQ(CL_ERR_RPC_CAPACITY, handle.second->playback(2, events));
    EXPECT_EQ(2u, peer.state().sequenceCalls.load());
}
TEST_P(TailDeadline, ReleaseAndFinalFacadeDropRetainTailChildAndNeverTouchReturnedOutput)
{
    test::NativePeer peer(GetParam(), true);
    peer.state().keeperCount = 2;
    peer.state().blockSequences = true;
    peer.state().delaySequenceMask = 1;
    peer.portal.RPC_POLICY.tail_ms = 300;
    peer.portal.RPC_POLICY.attempt_ms = 40;
    auto owner = ChronologClientImpl::GetClientImplInstance(peer.portal, WRITER_MODE, ClientQueryServiceConf{});
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto handle = owner->AcquireStory("tail", "a");
    ASSERT_EQ(CL_SUCCESS, handle.first);
    auto retained = std::weak_ptr<ClientRpcContext>(owner->context());
    auto wrapper = std::make_unique<Client>(peer.portal);
    owner.reset(); // The public Client is the last facade.
    std::vector<Event> events;
    ASSERT_EQ(CL_ERR_PARTIAL_RESULT, playbackWhenAdmitted(handle.second, 2, events, peer.state()));
    ASSERT_EQ(1u, events.size());
    // Retry only definite pre-dispatch capacity refusal while the responsive
    // child retires. The blocked child keeps its native slot throughout.
    int released = CL_ERR_RPC_CAPACITY;
    ASSERT_TRUE(peer.until(
            [&]
            {
                auto forwards = peer.state().releases.load();
                released = wrapper->ReleaseStory("tail", "a");
                if(released == CL_ERR_RPC_CAPACITY)
                {
                    EXPECT_EQ(forwards, peer.state().releases.load());
                    return false;
                }
                return true;
            }));
    ASSERT_EQ(CL_SUCCESS, released);
    ASSERT_EQ(1u, peer.state().releases.load());
    wrapper.reset();
    EXPECT_EQ(1u, peer.state().sequenceReplies.load());
    EXPECT_FALSE(retained.expired());
    events.emplace_back(999, 7, 3, "caller-owned sentinel");
    auto original = events.front().sequence();
    peer.state().blockSequences = false;
    ASSERT_TRUE(peer.until([&] { return peer.state().sequenceReplies.load() == 2; }));
    ASSERT_EQ(2u, events.size());
    EXPECT_EQ(original, events.front().sequence());
    EXPECT_EQ("caller-owned sentinel", events.back().log_record());
    auto context = retained.lock();
    ASSERT_NE(context, nullptr);
    EXPECT_FALSE(context->cleanupComplete.load());
}
INSTANTIATE_TEST_SUITE_P(Transports, TailDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
