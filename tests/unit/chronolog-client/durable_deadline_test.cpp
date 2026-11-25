// StoryHandle::log_event(record, options, event_time) through the Client's
// owned RPC runtime, against a keeper in a child native peer. All native peers
// here use loopback. For OFI TCP select its local interface explicitly when
// provider defaults choose another NIC: FI_TCP_IFACE=lo.
#include "ClientNativePeer.h"
#include "ChronologClientImpl.h"
#include <gtest/gtest.h>
#include <cstring>
#include <thread>
using namespace chronolog;
using namespace std::chrono_literals;
namespace
{
using Clock = std::chrono::steady_clock;
LogEventOptions durable(uint32_t timeout_ms = 2000, uint32_t attempts = 3)
{
    LogEventOptions options;
    options.durability = Durability::Durable;
    options.timeout_ms = timeout_ms;
    options.attempts = attempts;
    return options;
}
// The Connect and AcquireStory just before can still hold the Client's two RPC
// slots. A write refused for capacity sent nothing, so it is tried again.
int logWhenAdmitted(StoryHandle* handle,
                    std::string const& record,
                    LogEventOptions const& options,
                    uint64_t& event_time,
                    test::PeerState& peer,
                    Clock::duration* elapsed = nullptr)
{
    int result = CL_ERR_RPC_CAPACITY;
    EXPECT_TRUE(test::until(
            [&]
            {
                auto sent = peer.durableCalls.load() + peer.records.load();
                auto started = Clock::now();
                result = handle->log_event(record, options, event_time);
                auto duration = Clock::now() - started;
                if(result == CL_ERR_RPC_CAPACITY)
                {
                    EXPECT_EQ(sent, peer.durableCalls.load() + peer.records.load());
                    EXPECT_EQ(0u, event_time);
                    return false;
                }
                if(elapsed)
                    *elapsed = duration;
                return true;
            }));
    return result;
}
} // namespace
class DurableDeadline: public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        peer = std::make_unique<test::NativePeer>(GetParam(), true);
        peer->state().keeperCount = 1;
        owner = ChronologClientImpl::GetClientImplInstance(peer->portal, WRITER_MODE, ClientQueryServiceConf{});
        ASSERT_EQ(CL_SUCCESS, owner->Connect());
        auto acquired = owner->AcquireStory("chron", "a");
        ASSERT_EQ(CL_SUCCESS, acquired.first);
        handle = acquired.second;
    }
    test::PeerState& state() { return peer->state(); }
    std::unique_ptr<test::NativePeer> peer;
    std::shared_ptr<ChronologClientImpl> owner;
    StoryHandle* handle = nullptr;
};
// The keeper stores the event once and answers CL_SUCCESS: the call returns
// that, with the timestamp the keeper received, and sends the names with it.
TEST_P(DurableDeadline, SuccessReturnsTheEventTime)
{
    uint64_t event_time = 0;
    EXPECT_EQ(CL_SUCCESS, logWhenAdmitted(handle, "durable record", durable(), event_time, state()));
    ASSERT_EQ(1u, state().durableCalls.load());
    ASSERT_EQ(1u, state().eventCount.load());
    EXPECT_EQ(1, state().events[0].durable);
    EXPECT_EQ(event_time, state().events[0].time);
    EXPECT_STREQ("durable record", state().events[0].record);
    EXPECT_STREQ("chron", state().durableChronicle);
    EXPECT_STREQ("a", state().durableStory);
    EXPECT_EQ(0u, state().records.load());
}
// A keeper refusal is returned as is with event_time 0, except
// CL_ERR_DURABILITY_UNKNOWN, which keeps the timestamp to look the event up.
TEST_P(DurableDeadline, KeeperStatusesPassThrough)
{
    for(int code: {int(CL_ERR_DURABILITY_FULL), int(CL_ERR_DURABILITY_UNAVAILABLE)})
    {
        state().durableStatus = code;
        uint64_t event_time = 1;
        EXPECT_EQ(code, logWhenAdmitted(handle, "record", durable(), event_time, state()));
        EXPECT_EQ(0u, event_time);
    }
    state().durableStatus = CL_ERR_DURABILITY_UNKNOWN;
    uint64_t event_time = 0;
    EXPECT_EQ(CL_ERR_DURABILITY_UNKNOWN, logWhenAdmitted(handle, "record", durable(), event_time, state()));
    EXPECT_EQ(event_time, state().events[2].time);
}
// An attempt that gets no answer is sent again with the identical event, so
// the keeper can drop the copy; the answer to the second attempt is the result.
TEST_P(DurableDeadline, UnansweredAttemptIsSentAgainWithTheSameIdentity)
{
    state().durableDropReplies = 1;
    uint64_t event_time = 0;
    EXPECT_EQ(CL_SUCCESS, logWhenAdmitted(handle, "record", durable(200, 3), event_time, state()));
    ASSERT_EQ(2u, state().durableCalls.load());
    auto const& first = state().events[0];
    auto const& second = state().events[1];
    EXPECT_EQ(first.time, second.time);
    EXPECT_EQ(first.clientId, second.clientId);
    EXPECT_EQ(first.eventIndex, second.eventIndex);
    EXPECT_EQ(event_time, first.time);
}
// With no attempt answered the call returns CL_ERR_DURABILITY_UNKNOWN with
// the timestamp, after attempts * timeout_ms and not much later.
TEST_P(DurableDeadline, NoAnswerIsUnknownWithinTheWholeBudget)
{
    state().durableDropReplies = 100;
    uint64_t event_time = 0;
    Clock::duration elapsed{};
    EXPECT_EQ(CL_ERR_DURABILITY_UNKNOWN,
              logWhenAdmitted(handle, "record", durable(150, 3), event_time, state(), &elapsed));
    EXPECT_EQ(3u, state().durableCalls.load());
    EXPECT_EQ(event_time, state().events[0].time);
    EXPECT_GE(elapsed, 440ms);
    EXPECT_LT(elapsed, 1500ms);
}
// A refusal after an unanswered attempt cannot rule out that the first
// attempt stored the event.
TEST_P(DurableDeadline, RefusalAfterAnUnansweredAttemptIsUnknown)
{
    state().durableDropReplies = 1;
    state().durableStatus = CL_ERR_DURABILITY_UNAVAILABLE;
    uint64_t event_time = 0;
    EXPECT_EQ(CL_ERR_DURABILITY_UNKNOWN, logWhenAdmitted(handle, "record", durable(200, 3), event_time, state()));
    EXPECT_EQ(2u, state().durableCalls.load());
    EXPECT_NE(0u, event_time);
}
// Bad options and an empty durable record are refused with nothing sent.
TEST_P(DurableDeadline, InvalidOptionsAreRefusedBeforeSending)
{
    uint64_t event_time = 1;
    EXPECT_EQ(CL_ERR_INVALID_ARG, handle->log_event("record", durable(0, 3), event_time));
    EXPECT_EQ(CL_ERR_INVALID_ARG, handle->log_event("record", durable(1000, 0), event_time));
    EXPECT_EQ(CL_ERR_INVALID_ARG, handle->log_event("", durable(), event_time));
    LogEventOptions unknown_level;
    unknown_level.durability = static_cast<Durability>(7);
    EXPECT_EQ(CL_ERR_INVALID_ARG, handle->log_event("record", unknown_level, event_time));
    EXPECT_EQ(0u, event_time);
    EXPECT_EQ(0u, state().durableCalls.load());
    EXPECT_EQ(0u, state().records.load());
}
// Accepted is the append path: record_event, and the keeper's status where
// log_event(record) would return 0.
TEST_P(DurableDeadline, AcceptedReturnsTheKeeperStatus)
{
    uint64_t event_time = 0;
    EXPECT_EQ(CL_SUCCESS, logWhenAdmitted(handle, "first", LogEventOptions{}, event_time, state()));
    EXPECT_EQ(event_time, state().events[0].time);
    EXPECT_EQ(0, state().events[0].durable);
    state().recordStatus = -15;
    uint64_t refused_time = 1;
    EXPECT_EQ(-15, logWhenAdmitted(handle, "second", LogEventOptions{}, refused_time, state()));
    EXPECT_EQ(0u, refused_time);
    EXPECT_EQ(2u, state().records.load());
    EXPECT_EQ(0u, state().durableCalls.load());
}
// The attempts of one durable write share one RPC slot: while it is on its
// later attempts an append still finds the other slot.
TEST_P(DurableDeadline, RetriesKeepToOneSlot)
{
    state().durableDropReplies = 100;
    uint64_t durable_time = 0;
    int durable_status = CL_ERR_UNKNOWN;
    std::thread writer(
            [&] { durable_status = logWhenAdmitted(handle, "durable", durable(300, 3), durable_time, state()); });
    ASSERT_TRUE(peer->until([&] { return state().durableCalls.load() >= 2; }));
    uint64_t accepted_time = 0;
    EXPECT_EQ(CL_SUCCESS, handle->log_event("accepted", LogEventOptions{}, accepted_time));
    writer.join();
    EXPECT_EQ(CL_ERR_DURABILITY_UNKNOWN, durable_status);
    EXPECT_EQ(3u, state().durableCalls.load());
    EXPECT_EQ(CL_SUCCESS, handle->log_event("after", LogEventOptions{}, accepted_time));
    EXPECT_EQ(2u, state().records.load());
}
// Every attempt's timed forward has returned before the write ends, so a write
// that got no answer gives its slot back: three in a row on a client with two
// slots all reach the keeper, and an append after them is answered.
TEST_P(DurableDeadline, UnansweredWritesGiveTheirSlotsBack)
{
    state().durableDropReplies = 100;
    for(int i = 0; i < 3; ++i)
    {
        uint64_t event_time = 0;
        EXPECT_EQ(CL_ERR_DURABILITY_UNKNOWN, logWhenAdmitted(handle, "durable", durable(100, 1), event_time, state()));
        EXPECT_NE(0u, event_time);
    }
    EXPECT_EQ(3u, state().durableCalls.load());
    uint64_t accepted_time = 0;
    EXPECT_EQ(CL_SUCCESS, logWhenAdmitted(handle, "accepted", LogEventOptions{}, accepted_time, state()));
    EXPECT_EQ(1u, state().records.load());
}
INSTANTIATE_TEST_SUITE_P(Transports, DurableDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
