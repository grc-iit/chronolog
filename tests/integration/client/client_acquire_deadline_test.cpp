#include "../../unit/chrono-visor/admin_test_fixture.h"
#include <AcquireStoryResponseMsg.h>
struct NativeAcquire final: rpc::OwnedRpcWork<std::shared_ptr<AcquireStoryResponseMsg>>
{
    std::shared_ptr<tl::engine> engine;
    std::string address;
    tl::remote_procedure remote;
    std::string story = "story";
    rpc::RpcCallState native;
    rpc::RpcResult<std::shared_ptr<AcquireStoryResponseMsg>> run(rpc::OperationContext& context) override
    {
        native.bind(*engine, remote, address, 40);
        auto cap = context.deadline().cappedAttempt(120ms);
        if(!cap || !context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        auto value = native.callable()
                             .timed(*cap, ClientId{100}, std::string("chronicle"), story)
                             .as<AcquireStoryResponseMsg>();
        auto reply = std::make_shared<AcquireStoryResponseMsg>(value);
        native.clearKnown();
        return {value.getErrorCode(), true, std::move(reply)};
    }
};
class ClientAcquireDeadline: public AdminDeadline
{
protected:
    std::shared_ptr<tl::engine> clientEngine;
    tl::remote_procedure acquireRpc;
    // Match the accepted native B01 owner: a dedicated RPC stream also keeps
    // native timed-call callbacks off the test thread that sleeps on std clocks.
    void SetUp() override
    {
        AdminDeadline::SetUp();
        clientEngine = std::make_shared<tl::engine>(protocol, THALLIUM_CLIENT_MODE, true, 1);
        acquireRpc = clientEngine->define("AcquireStory");
    }
    rpc::RpcResult<std::shared_ptr<AcquireStoryResponseMsg>>
    acquire(VisorClientPortal& portal, rpc::RpcRuntime& caller, std::chrono::milliseconds budget)
    {
        auto deadline = rpc::RpcDeadline::after(budget);
        auto token = caller.reserve(deadline);
        auto work = std::make_shared<NativeAcquire>();
        work->engine = clientEngine;
        work->remote = acquireRpc;
        work->address = std::string(portal.core()->engine->self());
        return caller.invoke<std::shared_ptr<AcquireStoryResponseMsg>>(std::move(token), std::move(work));
    }
};
TEST_P(ClientAcquireDeadline, NativePortalSerializesOnlyConfirmedResponsiveRoster)
{
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto begin = std::chrono::steady_clock::now();
    auto result = acquire(portal, caller, 220ms);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 260ms);
    ASSERT_TRUE(result.reply);
    auto& response = **result.reply;
    EXPECT_EQ(response.getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(response.getKeepers().size(), 1u);
    EXPECT_EQ(to_string(response.getKeepers()[0]), to_string(keeper(3).getRecordingServiceId()));
    EXPECT_EQ(to_string(response.getPlayer()), to_string(service(5)));
    EXPECT_EQ(caller.drain(rpc::RpcDeadline::after(200ms)), CL_SUCCESS);
}
TEST_P(ClientAcquireDeadline, LostClientReplyLeavesDuplicateAcquireErrorWithEmptyRoster)
{
    delays[0] = 70;
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto result = acquire(portal, caller, 25ms);
    EXPECT_FALSE(result.reply);
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_TRUE(result.may_have_dispatched);
    std::this_thread::sleep_for(150ms);
    auto duplicate = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(duplicate.getErrorCode(), CL_ERR_ACQUIRED);
    EXPECT_TRUE(duplicate.getKeepers().empty());
    EXPECT_FALSE(result.reply);
}
TEST_P(ClientAcquireDeadline, SaturationRejectsBeforeOwningRequestAndCallerWaitIsBounded)
{
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(500ms);
    auto tokens = occupy(32, deadline);
    ASSERT_EQ(tokens.size(), 32u);
    auto before = portal.core()->incomingRequests.load();
    auto begin = std::chrono::steady_clock::now();
    auto result = acquire(portal, caller, 120ms);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    EXPECT_FALSE(result.reply);
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(portal.core()->incomingRequests.load(), before);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    tokens.clear();
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
}
INSTANTIATE_TEST_SUITE_P(Native, ClientAcquireDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
