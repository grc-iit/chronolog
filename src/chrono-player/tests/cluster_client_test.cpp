#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include "chrono-player/adapter/ClusterClient.h"

namespace chronolog::player
{
namespace
{
class CatalogSnapshot final: public internal::v1::Cluster::Service
{
public:
    std::atomic<StoryId> story{0};
    std::atomic<int> calls{0};
    std::atomic<uint64_t> epoch{1};
    std::atomic<uint64_t> revision{0};
    std::atomic<bool> fail{false};
    grpc::Status Register(grpc::ServerContext*,
                          const internal::v1::RegisterRequest*,
                          internal::v1::RegisterResponse* response) override
    {
        ++calls;
        if(fail)
            return {grpc::StatusCode::UNAVAILABLE, "visor unavailable"};
        if(auto id = story.load(); id != 0)
        {
            auto* update = response->add_routes();
            update->set_story_id(id);
            update->mutable_route()->set_epoch(epoch);
            update->set_revision(revision);
            auto* keeper = update->mutable_route()->add_keepers();
            keeper->set_process_id("keeper-1");
            keeper->set_endpoint("keeper:50052");
        }
        return grpc::Status::OK;
    }
};

TEST(PlayerClusterClientTest, LearnsNewStoryOnImmediateCacheMiss)
{
    CatalogSnapshot catalog;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&catalog);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ClusterClient player(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()),
                         {"player-1", "instance", "player:50054", ProcessRole::Player},
                         std::chrono::milliseconds(1000));
    int learned = 0;
    player.onRoute(
            [&](const Route& route)
            {
                EXPECT_EQ(route.epoch, 1);
                ++learned;
            });
    ASSERT_TRUE(player.registerSelf().ok());
    ASSERT_EQ(catalog.calls, 1);

    catalog.story = 42;
    auto route = player.route(42);
    ASSERT_TRUE(route.ok()) << route.status();
    EXPECT_EQ(route->keepers, (std::vector<KeeperRef>{{"keeper-1", "keeper:50052"}}));
    EXPECT_EQ(catalog.calls, 2);
    EXPECT_EQ(learned, 1);
    ASSERT_TRUE(player.route(42).ok());
    EXPECT_EQ(catalog.calls, 2);

    EXPECT_EQ(player.route(99).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ(catalog.calls, 3);
    catalog.story = 99;
    ASSERT_TRUE(player.route(99).ok());
    EXPECT_EQ(catalog.calls, 4);

    catalog.fail = true;
    EXPECT_EQ(player.route(100).status().code(), absl::StatusCode::kUnavailable);
    catalog.fail = false;
    catalog.story = 100;
    ASSERT_TRUE(player.route(100).ok());
    EXPECT_EQ(catalog.calls, 6);
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
}
TEST(PlayerClusterClientTest, RejectsLowerRevisionAndEpochSnapshots)
{
    CatalogSnapshot catalog;
    catalog.story = 42;
    catalog.epoch = 3;
    catalog.revision = 10;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&catalog);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ClusterClient player(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()),
                         {"player-1", "instance", "player:50054", ProcessRole::Player},
                         std::chrono::milliseconds(1000));
    ASSERT_TRUE(player.registerSelf().ok());
    catalog.revision = 9;
    catalog.epoch = 4;
    ASSERT_TRUE(player.registerSelf().ok());
    ASSERT_TRUE(player.route(42).ok());
    EXPECT_EQ(player.route(42)->epoch, 3);
    catalog.revision = 11;
    catalog.epoch = 2;
    ASSERT_TRUE(player.registerSelf().ok());
    EXPECT_EQ(player.route(42)->epoch, 3);
    catalog.epoch = 4;
    ASSERT_TRUE(player.registerSelf().ok());
    EXPECT_EQ(player.route(42)->epoch, 4);
    catalog.story = 99;
    catalog.revision = 9;
    ASSERT_TRUE(player.registerSelf().ok());
    EXPECT_EQ(player.route(99).status().code(), absl::StatusCode::kNotFound);
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
}
} // namespace
} // namespace chronolog::player
