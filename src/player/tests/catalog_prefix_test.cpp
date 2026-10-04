#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "common/rpc/Channel.h"
#include "player/adapter/StoryCatalog.h"

namespace chronolog::player
{
namespace
{
class Catalog final: public v1::Catalog::Service
{
public:
    std::vector<StoryId> stories;
    uint64_t revision{};
    bool limit_exceeded{};
    int code{};
    grpc::Status ListStoriesByPrefix(grpc::ServerContext*,
                                     const v1::ListStoriesByPrefixRequest*,
                                     v1::ListStoriesByPrefixResponse* response) override
    {
        response->mutable_status()->set_code(code);
        response->set_revision(revision);
        response->set_limit_exceeded(limit_exceeded);
        for(const auto id: stories) response->add_stories()->set_story_id(id);
        return grpc::Status::OK;
    }
};

class PlayerCatalogPrefixTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        grpc::ServerBuilder builder;
        chronolog::rpc::applyServerPolicy(builder);
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&catalog);
        server = builder.BuildAndStart();
        ASSERT_NE(server, nullptr);
        client = std::make_unique<CatalogClient>(rpc::peerChannel("127.0.0.1:" + std::to_string(port)),
                                                 std::chrono::seconds(5));
    }
    void TearDown() override { server->Shutdown(std::chrono::system_clock::now()); }
    Catalog catalog;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<CatalogClient> client;
};

TEST_F(PlayerCatalogPrefixTest, ResolvePrefixReturnsTheStoriesAndTheRevision)
{
    catalog.stories = {3, 5};
    catalog.revision = 9;
    auto resolved = client->resolvePrefix("c/a", 8);
    ASSERT_TRUE(resolved.ok()) << resolved.status();
    EXPECT_EQ(resolved->revision, 9u);
    EXPECT_EQ(resolved->stories, (std::vector<StoryId>{3, 5}));
    catalog.limit_exceeded = true;
    EXPECT_TRUE(absl::IsResourceExhausted(client->resolvePrefix("c/a", 1).status()));
    catalog.limit_exceeded = false;
    catalog.code = static_cast<int>(absl::StatusCode::kInvalidArgument);
    EXPECT_TRUE(absl::IsInvalidArgument(client->resolvePrefix("c/a", 0).status()));
}

TEST_F(PlayerCatalogPrefixTest, ConfirmingReadSeesAStoryCreatedAfterTheResolution)
{
    catalog.stories = {3, 5};
    catalog.revision = 9;
    auto resolved = client->resolvePrefix("c/a", 8);
    ASSERT_TRUE(resolved.ok());
    auto confirmed = client->confirmPrefix("c/a", 8, *resolved);
    ASSERT_TRUE(confirmed.ok()) << confirmed.status();
    EXPECT_FALSE(confirmed->created);
    catalog.stories = {3, 5, 7};
    catalog.revision = 12;
    confirmed = client->confirmPrefix("c/a", 8, *resolved);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_TRUE(confirmed->created);
    EXPECT_EQ(confirmed->current.revision, 12u);
    EXPECT_EQ(confirmed->current.stories, (std::vector<StoryId>{3, 5, 7}));
    EXPECT_FALSE(client->confirmPrefix("c/a", 8, confirmed->current)->created);
}

TEST_F(PlayerCatalogPrefixTest, ADestroyedStoryIsNotACreatedStory)
{
    catalog.stories = {3, 5};
    catalog.revision = 9;
    auto resolved = client->resolvePrefix("c/a", 8);
    ASSERT_TRUE(resolved.ok());
    catalog.stories = {5};
    catalog.revision = 10;
    auto confirmed = client->confirmPrefix("c/a", 8, *resolved);
    ASSERT_TRUE(confirmed.ok());
    EXPECT_FALSE(confirmed->created);
}
} // namespace
} // namespace chronolog::player
