// Deleting one PlaybackQueryRpcClient leaves the others of the same client
// working (#707, #743). A reader client holds one PlaybackQueryRpcClient route
// per player. Every route sends story_playback_request through the client
// context's procedures, and two players with the same provider id share that
// RPC ID on the client's engine. Before #740 the route's destructor
// deregistered it, and every other player's route then failed with
// HG_NOENTRY.
//
// The test stands up two fake playback services, each on an engine of its
// own with the same provider id, and a client context whose engine and
// procedures it sets up itself, over ofi+sockets on 127.0.0.1. Each request
// runs on the context's RpcRuntime, as ClientQueryService sends it.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <RpcRuntime.h>
#include <ServiceId.h>
#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <client_errcode.h>

#include "ClientQueryService.h"
#include "ClientRpcContext.h"
#include "PlaybackQueryRpcClient.h"

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kPlaybackProvider = 99;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console",
                                        "",
                                        chl::LogLevel::err,
                                        "playback_query_rpc_client_lifetime_test_logger");
        done = true;
    }
}

uint16_t portOf(std::string const& address)
{
    return static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
}

// Stands in for a player's PlaybackService: acknowledges a playback request
// with its query id and counts the requests it gets.
struct FakePlaybackService
{
    FakePlaybackService()
        : engine(std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1))
    {
        procedure = engine->define(
                "story_playback_request",
                [this](tl::request const& request,
                       chl::ServiceId const&,
                       uint32_t query,
                       chl::StoryId const&,
                       chl::ChronicleName const&,
                       chl::StoryName const&,
                       chl::chrono_time const&,
                       chl::chrono_time const&)
                {
                    ++requests;
                    request.respond(query);
                },
                kPlaybackProvider);
    }

    chl::ServiceId id() const
    {
        return chl::ServiceId(kProtocol, "127.0.0.1", portOf(engine->self()), kPlaybackProvider);
    }

    std::unique_ptr<tl::engine> engine;
    tl::remote_procedure procedure{};
    std::atomic<int> requests{0};
};

struct Request: chl::rpc::OwnedRpcWork<int>
{
    std::function<int(chl::rpc::OperationContext&)> body;
    explicit Request(std::function<int(chl::rpc::OperationContext&)> body)
        : body(std::move(body))
    {}
    chl::rpc::RpcResult<int> run(chl::rpc::OperationContext& operation) override
    {
        return {chl::CL_SUCCESS, false, body(operation)};
    }
};

class PlaybackQueryRpcClientLifetime: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        chl::ClientPortalServiceConf portal;
        portal.PROTO_CONF = kProtocol;
        portal.IP = "127.0.0.1";
        chl::ClientQueryServiceConf query{kProtocol, "127.0.0.1", 0, 57};
        context = std::make_shared<chl::ClientRpcContext>(portal, true, query);
        context->engine =
                std::make_shared<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        context->procedures = std::make_shared<chl::ClientRpcProcedures>(*context->engine);
        runtime = std::make_shared<chl::rpc::RpcRuntime>();
    }

    void TearDown() override
    {
        context->procedures.reset();
        context->engine->finalize();
        for(auto& player: players) player.engine->finalize();
    }

    // Sends one playback request for query through route on the runtime, and
    // returns the player's acknowledgement (the query id) or a negative code.
    int send(chl::PlaybackQueryRpcClient const& route, uint32_t query)
    {
        // where the player would send its response; the fake never does
        chl::ServiceId const receiver(kProtocol, "127.0.0.1", portOf(context->engine->self()), 57);
        auto playback =
                std::make_shared<chl::PlaybackQuery>(query,
                                                     std::chrono::steady_clock::now() + std::chrono::seconds(10),
                                                     7,
                                                     "chronicle",
                                                     "story",
                                                     1,
                                                     1000);
        auto retained = context;
        auto result = runtime->invoke<int>(
                chl::rpc::RpcDeadline::after(std::chrono::seconds(10)),
                std::make_shared<Request>(
                        [retained, playback, &route, receiver](chl::rpc::OperationContext& operation)
                        {
                            chl::rpc::RpcCallState native;
                            auto reply = route.send_story_playback_request(*retained,
                                                                           operation,
                                                                           native,
                                                                           receiver,
                                                                           *playback,
                                                                           5000);
                            return reply.reply ? static_cast<int>(*reply.reply) : reply.status;
                        }));
        return result.reply.value_or(result.status);
    }

    std::shared_ptr<chl::ClientRpcContext> context;
    std::shared_ptr<chl::rpc::RpcRuntime> runtime;
    FakePlaybackService players[2];
};

} // namespace

// The route of one player is deleted. The route of the other player still
// sends it a playback request, and the deleted route's player gets no more.
TEST_F(PlaybackQueryRpcClientLifetime, DeletingOneClientLeavesTheOtherWorking)
{
    auto leaving = chl::PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(players[0].id());
    auto staying = chl::PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(players[1].id());
    ASSERT_NE(leaving, nullptr);
    ASSERT_NE(staying, nullptr);
    ASSERT_EQ(send(*leaving, 1), 1);
    ASSERT_EQ(players[0].requests.load(), 1);

    leaving.reset();

    EXPECT_EQ(send(*staying, 2), 2) << "the route that stayed could not send its request";
    EXPECT_EQ(players[1].requests.load(), 1);
    EXPECT_EQ(players[0].requests.load(), 1);
}
