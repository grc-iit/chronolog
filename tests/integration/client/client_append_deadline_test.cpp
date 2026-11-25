#include "ClientNativePeer.h"
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "KeeperRecordingClient.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <thread>
using namespace chronolog;
using Clock = std::chrono::steady_clock;
namespace chronolog
{
struct KeeperRecordingClientTestAccess
{
    static void observe(KeeperRecordingClient& client,
                        std::shared_ptr<KeeperRecordingClient::AppendObservers const> observers)
    {
        client.appendObservers_ = std::move(observers);
    }
};
struct StoryHandleTestAccess
{
    static void observe(StoryHandle* raw, std::shared_ptr<KeeperRecordingClient::AppendObservers const> observers)
    {
        auto& handle = *static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(raw);
        for(auto* keeper: handle.storyKeepers) KeeperRecordingClientTestAccess::observe(*keeper, observers);
    }
};
} // namespace chronolog
namespace
{
struct Gate
{
    std::atomic<unsigned> entered{0};
    std::atomic<bool> released{false};
    void wait()
    {
        ++entered;
        while(!released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};
struct RawClock
{
    std::atomic<uint64_t> time{100};
    std::atomic<int> status{0};
    std::atomic<bool> blockNext{false};
    std::shared_ptr<Gate> sourceGate = std::make_shared<Gate>();
};
} // namespace
class ClientAppendDeadline: public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        peer = std::make_unique<test::NativePeer>(GetParam(), true);
        peer->portal.RPC_POLICY.control_ms = 1000;
        peer->portal.RPC_POLICY.append_ms = 40;
        raw = std::make_shared<RawClock>();
        owner = ChronologClientImpl::GetClientImplInstance(peer->portal,
                                                           WRITER_MODE,
                                                           ClientQueryServiceConf{},
                                                           [clock = raw]
                                                           {
                                                               if(clock->blockNext.exchange(false))
                                                                   clock->sourceGate->wait();
                                                               return TimestampResult{clock->status.load(),
                                                                                      clock->time.load()};
                                                           });
        context = owner->context();
        ASSERT_EQ(0, owner->Connect());
        auto acquired = owner->AcquireStory("c", "a");
        ASSERT_EQ(0, acquired.first);
        ASSERT_NE(nullptr, acquired.second);
        handle = acquired.second;
    }
    void TearDown() override
    {
        if(gate)
            gate->released = true;
        peer->state().blockRecords = false;
        raw->sourceGate->released = true;
        owner.reset();
        if(healthy)
        {
            EXPECT_TRUE(test::until([&] { return context->cleanupComplete.load(); }));
        }
        peer.reset();
    }
    std::shared_ptr<KeeperRecordingClient::AppendObservers> observers()
    {
        return std::make_shared<KeeperRecordingClient::AppendObservers>();
    }
    void install(std::shared_ptr<KeeperRecordingClient::AppendObservers> observer)
    {
        StoryHandleTestAccess::observe(handle, std::move(observer));
    }
    void expectNotSubmitted(AppendResult const& result, uint64_t timestamp = 100, uint32_t index = 1)
    {
        EXPECT_EQ(-22, result.status);
        EXPECT_EQ(AppendOutcome::NotSubmitted, result.outcome);
        EXPECT_EQ(timestamp, result.timestamp);
        EXPECT_EQ(index, result.event_index);
    }
    void blockedStage(std::string const& stage)
    {
        gate = std::make_shared<Gate>();
        auto observer = observers();
        auto wait = [owned = gate] { owned->wait(); };
        if(stage == "lookup")
            observer->beforeLookup = wait;
        if(stage == "afterLookup")
            observer->afterLookup = wait;
        if(stage == "dispatch")
            observer->beforeDispatch = wait;
        install(observer);
        auto started = Clock::now();
        auto result = handle->append_event(stage);
        EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
        expectNotSubmitted(result);
        ASSERT_EQ(1u, gate->entered.load());
        gate->released = true;
        owner.reset();
        ASSERT_TRUE(test::until([&] { return context->cleanupComplete.load(); }));
        EXPECT_EQ(0u, peer->state().records.load());
    }
    std::unique_ptr<test::NativePeer> peer;
    std::shared_ptr<ChronologClientImpl> owner;
    std::shared_ptr<ClientRpcContext> context;
    std::shared_ptr<RawClock> raw;
    std::shared_ptr<Gate> gate;
    StoryHandle* handle = nullptr;
    bool healthy = true;
};
TEST_P(ClientAppendDeadline, AcknowledgedReplyPreservesWireIdentityAndSingleChosenRoute)
{
    auto result = handle->append_event("accepted");
    EXPECT_EQ(0, result.status);
    EXPECT_EQ(AppendOutcome::Acknowledged, result.outcome);
    EXPECT_EQ(100u, result.timestamp);
    EXPECT_EQ(1u, result.event_index);
    ASSERT_EQ(1u, peer->state().eventCount.load());
    auto const& event = peer->state().events[0];
    EXPECT_EQ(100u, event.time);
    EXPECT_EQ(1u, event.eventIndex);
    EXPECT_EQ(1u, event.storyId);
    EXPECT_EQ(owner->client_id(), event.clientId);
    EXPECT_EQ(1u, event.route); // timestamp100 modulo three keepers
    EXPECT_STREQ("accepted", event.record);
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, ExplicitKeeperRejectionPreservesStatusAndIdentityWithoutRetry)
{
    peer->state().recordStatus = -9;
    auto result = handle->append_event("rejected");
    EXPECT_EQ(-9, result.status);
    EXPECT_EQ(AppendOutcome::Rejected, result.outcome);
    EXPECT_EQ(100u, result.timestamp);
    EXPECT_EQ(1u, result.event_index);
    EXPECT_EQ(1u, peer->state().records.load());
    EXPECT_EQ(0u, peer->state().eventCount.load());
}
TEST_P(ClientAppendDeadline, LegacyAppendDoesNotWaitForLateReply)
{
    peer->state().recordDelayMs = 150;
    auto started = Clock::now();
    auto timestamp = handle->log_event("late");
    auto elapsed = Clock::now() - started;
    EXPECT_EQ(0u, timestamp);
    EXPECT_LT(elapsed, std::chrono::milliseconds(120));
    EXPECT_TRUE(peer->until([&] { return peer->state().eventCount.load() == 1; }));
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, LateReplyReportsUnconfirmedStampedIdentityAndNeverResends)
{
    peer->state().recordDelayMs = 150;
    auto started = Clock::now();
    auto result = handle->append_event("late");
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_EQ(-23, result.status);
    EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    EXPECT_EQ(100u, result.timestamp);
    EXPECT_EQ(1u, result.event_index);
    ASSERT_TRUE(peer->until([&] { return peer->state().eventCount.load() == 1; }));
    auto const& event = peer->state().events[0];
    EXPECT_EQ(100u, event.time);
    EXPECT_EQ(1u, event.eventIndex);
    EXPECT_EQ(1u, event.route);
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, LostReplyReportsUnconfirmedWithoutSecondEventOrRoute)
{
    peer->state().dropRecordReply = true;
    auto started = Clock::now();
    auto result = handle->append_event("lost reply");
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_EQ(-23, result.status);
    EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    EXPECT_EQ(100u, result.timestamp);
    EXPECT_EQ(1u, result.event_index);
    ASSERT_TRUE(peer->until([&] { return peer->state().eventCount.load() == 1; }));
    EXPECT_EQ(100u, peer->state().events[0].time);
    EXPECT_EQ(1u, peer->state().events[0].eventIndex);
    EXPECT_EQ(1u, peer->state().events[0].route);
    EXPECT_EQ(1u, peer->state().records.load());
}
// The timed forward of an unanswered append has returned before its job ends,
// so the job gives its slot back. Three appends in a row go unanswered on a
// client with two slots, and the next one is acknowledged. Each append that
// finds both slots still busy is refused with nothing sent and tried again.
TEST_P(ClientAppendDeadline, UnansweredAppendsGiveTheirSlotsBack)
{
    peer->state().dropRecordReply = true;
    auto append = [&](std::string const& record)
    {
        AppendResult result{};
        EXPECT_TRUE(test::until(
                [&]
                {
                    result = handle->append_event(record);
                    return result.status != CL_ERR_RPC_CAPACITY;
                }));
        return result;
    };
    for(int i = 0; i < 3; ++i)
    {
        auto result = append("lost reply " + std::to_string(i));
        EXPECT_EQ(CL_ERR_OUTCOME_UNCONFIRMED, result.status);
        EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    }
    peer->state().dropRecordReply = false;
    auto answered = append("answered");
    EXPECT_EQ(CL_SUCCESS, answered.status);
    EXPECT_EQ(AppendOutcome::Acknowledged, answered.outcome);
    EXPECT_EQ(4u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, ExpiryBeforeLookupDoesNotForwardAfterWorkerResumes) { blockedStage("lookup"); }
TEST_P(ClientAppendDeadline, ExpiryAfterLookupDoesNotForwardAfterWorkerResumes) { blockedStage("afterLookup"); }
TEST_P(ClientAppendDeadline, ExpiryAfterCallablePreparationDoesNotForwardAfterWorkerResumes)
{
    blockedStage("dispatch");
}
TEST_P(ClientAppendDeadline, ZeroClockDoesNotSendOrInventIdentity)
{
    raw->time = 0;
    auto result = handle->append_event("zero clock");
    EXPECT_EQ(-25, result.status);
    EXPECT_EQ(AppendOutcome::NotSubmitted, result.outcome);
    EXPECT_EQ(0u, result.timestamp);
    EXPECT_EQ(0u, result.event_index);
    EXPECT_EQ(0u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, FailedClockDoesNotSendOrInventIdentity)
{
    raw->time = 900;
    raw->status = -1;
    auto result = handle->append_event("failed clock");
    EXPECT_EQ(-25, result.status);
    EXPECT_EQ(AppendOutcome::NotSubmitted, result.outcome);
    EXPECT_EQ(0u, result.timestamp);
    EXPECT_EQ(0u, result.event_index);
    EXPECT_EQ(0u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, ExhaustedClockDoesNotSendOrWrapIdentity)
{
    raw->time = std::numeric_limits<uint64_t>::max();
    auto last = handle->append_event("last");
    EXPECT_EQ(AppendOutcome::Acknowledged, last.outcome);
    EXPECT_EQ(1u, last.event_index);
    auto exhausted = handle->append_event("exhausted");
    EXPECT_EQ(-26, exhausted.status);
    EXPECT_EQ(AppendOutcome::NotSubmitted, exhausted.outcome);
    EXPECT_EQ(0u, exhausted.timestamp);
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, LegacyAcknowledgementReturnsAssignedTimestamp)
{
    EXPECT_EQ(100u, handle->log_event("legacy accepted"));
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, LegacyRejectionReturnsZero)
{
    peer->state().recordStatus = -9;
    EXPECT_EQ(0u, handle->log_event("legacy rejected"));
    EXPECT_EQ(1u, peer->state().records.load());
    EXPECT_EQ(0u, peer->state().eventCount.load());
}
TEST_P(ClientAppendDeadline, ClockContentionUsesEntryBudgetWithoutChargingNativeSlotsOrAdvancingIdentity)
{
    raw->blockNext = true;
    auto first = std::async(std::launch::async, [&] { return handle->log_event("blocked clock"); });
    ASSERT_TRUE(test::until([&] { return raw->sourceGate->entered.load() == 1; }));
    auto started = Clock::now();
    auto second = handle->append_event("contending clock");
    auto elapsed = Clock::now() - started;
    raw->sourceGate->released = true;
    EXPECT_EQ(0u, first.get());
    expectNotSubmitted(second, 0, 0);
    EXPECT_GE(elapsed, std::chrono::milliseconds(35));
    EXPECT_LT(elapsed, std::chrono::milliseconds(120));
    EXPECT_EQ(0u, peer->state().records.load());
    auto next = handle->append_event("after clock contention");
    EXPECT_EQ(AppendOutcome::Acknowledged, next.outcome);
    EXPECT_EQ(100u, next.timestamp);
    EXPECT_EQ(1u, next.event_index);
    EXPECT_EQ(1u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, ConcurrentCallsKeepSeparateEntryBudgetsAndCapacity)
{
    gate = std::make_shared<Gate>();
    auto observer = observers();
    observer->beforeLookup = [owned = gate] { owned->wait(); };
    install(observer);
    auto first = std::async(std::launch::async, [&] { return handle->append_event("first"); });
    ASSERT_TRUE(test::until([&] { return gate->entered.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto started = Clock::now();
    auto second = std::async(std::launch::async, [&] { return handle->append_event("second"); });
    ASSERT_TRUE(test::until([&] { return gate->entered.load() == 2; }));
    auto overflow = handle->append_event("capacity");
    EXPECT_EQ(-24, overflow.status);
    EXPECT_EQ(AppendOutcome::NotSubmitted, overflow.outcome);
    EXPECT_EQ(102u, overflow.timestamp);
    EXPECT_EQ(3u, overflow.event_index);
    expectNotSubmitted(first.get(), 100, 1);
    expectNotSubmitted(second.get(), 101, 2);
    EXPECT_GE(Clock::now() - started, std::chrono::milliseconds(35));
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    gate->released = true;
    owner.reset();
    ASSERT_TRUE(test::until([&] { return context->cleanupComplete.load(); }));
    EXPECT_EQ(0u, peer->state().records.load());
}
TEST_P(ClientAppendDeadline, ReleaseAfterPublicTimeoutRetainsRealHandleAndOwnedPayloadThroughSettlement)
{
    gate = std::make_shared<Gate>();
    auto observer = observers();
    observer->afterNative = [owned = gate] { owned->wait(); };
    install(observer);
    std::string payload(1024 * 1024, 'x');
    payload.back() = 'z';
    auto result = handle->append_event(payload);
    EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    ASSERT_EQ(1u, gate->entered.load());
    payload.assign("caller reused storage");
    payload.shrink_to_fit();
    EXPECT_EQ(0, owner->ReleaseStory("c", "a"));
    EXPECT_EQ(1u, context->storyteller->retainedHandleCount());
    ASSERT_EQ(1u, peer->state().eventCount.load());
    EXPECT_EQ(1024u * 1024u, peer->state().events[0].size);
    EXPECT_EQ('x', peer->state().events[0].record[0]);
    EXPECT_EQ('z', peer->state().events[0].last);
    gate->released = true;
    EXPECT_TRUE(test::until([&] { return context->storyteller->retainedHandleCount() == 0; }));
}
TEST_P(ClientAppendDeadline, LastOwnerDropAfterTimeoutKeepsNativeOwnerUntilKnownReplyWorkSettles)
{
    auto wrapper = std::make_unique<Client>(peer->portal);
    owner.reset(); // The public wrapper is now the last admitted facade.
    gate = std::make_shared<Gate>();
    auto observer = observers();
    observer->afterNative = [owned = gate] { owned->wait(); };
    install(observer);
    auto result = handle->append_event("retained");
    EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    ASSERT_EQ(1u, gate->entered.load());
    auto started = Clock::now();
    wrapper.reset();
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(120));
    EXPECT_FALSE(context->cleanupComplete.load());
    auto replacement = ChronologClientImpl::GetClientImplInstance(peer->portal, WRITER_MODE, ClientQueryServiceConf{});
    EXPECT_EQ(context.get(), replacement->context().get());
    EXPECT_EQ(nullptr, replacement->runtime());
    replacement.reset();
    gate->released = true;
    EXPECT_TRUE(test::until([&] { return context->cleanupComplete.load(); }));
}
// The keeper holds the request without answering. The append's timed forward
// is cancelled and returns, so the job ends and gives back its slot, the story
// handle and the payload while the keeper still holds the request. The keeper
// later stores the payload the client copied before the timeout.
TEST_P(ClientAppendDeadline, BlockedKeeperDoesNotHoldTheHandleAfterTheForwardReturns)
{
    auto wrapper = std::make_unique<Client>(peer->portal);
    owner.reset();
    peer->state().blockRecords = true;
    std::string payload = "owned after timeout";
    auto result = handle->append_event(payload);
    EXPECT_EQ(AppendOutcome::Unconfirmed, result.outcome);
    EXPECT_EQ(100u, result.timestamp);
    EXPECT_EQ(1u, peer->state().records.load());
    payload.assign("reused");
    EXPECT_EQ(0, wrapper->ReleaseStory("c", "a"));
    EXPECT_TRUE(test::until([&] { return context->storyteller->retainedHandleCount() == 0; }));
    EXPECT_EQ(0u, peer->state().eventCount.load());
    peer->state().blockRecords = false;
    ASSERT_TRUE(peer->until([&] { return peer->state().eventCount.load() == 1; }));
    EXPECT_STREQ("owned after timeout", peer->state().events[0].record);
    EXPECT_EQ(1u, peer->state().records.load());
    wrapper.reset();
    EXPECT_TRUE(test::until([&] { return context->cleanupComplete.load(); }));
}
INSTANTIATE_TEST_SUITE_P(Native, ClientAppendDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
