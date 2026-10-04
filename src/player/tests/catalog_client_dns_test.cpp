#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "rpc/FlakyResolver.h"
#include "rpc/Channel.h"
#include "chrono-player/adapter/StoryCatalog.h"

namespace chronolog::player
{
namespace
{
class Catalog final: public v1::Catalog::Service
{
public:
    grpc::Status GetStory(grpc::ServerContext*, const v1::GetStoryRequest*, v1::GetStoryResponse* response) override
    {
        response->mutable_story()->set_story_id(1);
        return grpc::Status::OK;
    }
};

// FLAKE-2: the Player's first Read after a container joined the network hit one failed lookup and
// answered UNAVAILABLE for the rest of the 30 s re-resolution window.
TEST(PlayerCatalogClientTest, EnsureLiveRetriesATransientNameResolutionFailureWithinItsDeadline)
{
    Catalog catalog;
    grpc::ServerBuilder builder;
    chronolog::rpc::applyServerPolicy(builder);
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&catalog);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    rpc::test::failing_lookups = 1;
    CatalogClient client(rpc::peerChannel("visor.test:" + std::to_string(port)), std::chrono::seconds(5));
    EXPECT_TRUE(client.ensureLive(1).ok());
    server->Shutdown(std::chrono::system_clock::now());
}

// FLAKE-2: aardvark-dns can stay silent for many seconds when another container joins the network. A
// Player that connected at startup must not need the resolver for the Reads that follow.
TEST(PlayerCatalogClientTest, ConnectsAtConstructionSoAResolverOutageDoesNotFailReads)
{
    Catalog catalog;
    grpc::ServerBuilder builder;
    chronolog::rpc::applyServerPolicy(builder);
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&catalog);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    rpc::test::failing_lookups = 0;
    auto channel = rpc::peerChannel("visor.test:" + std::to_string(port));
    CatalogClient client(channel, std::chrono::milliseconds(500));
    ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(5)));
    rpc::test::failing_lookups = 1 << 20;
    EXPECT_TRUE(client.ensureLive(1).ok());
    rpc::test::failing_lookups = 0;
    server->Shutdown(std::chrono::system_clock::now());
}
} // namespace
} // namespace chronolog::player
