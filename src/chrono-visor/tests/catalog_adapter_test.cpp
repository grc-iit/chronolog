// CatalogService end to end through a real in-process gRPC channel over the
// in-memory store.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "adapter/CatalogService.h"
#include "adapter/WorkerPool.h"
#include "catalog/InMemoryMetadataStore.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{
namespace
{

using namespace std::chrono_literals;
using testing::twoKeeperTopology;

constexpr int kNotFound = 5;
constexpr int kAlreadyExists = 6;
constexpr int kFailedPrecondition = 9;

class catalog_adapter: public ::testing::Test
{
protected:
    void SetUp() override
    {
        membership_ =
                std::make_unique<StaticRouteMembership>(twoKeeperTopology(), 1, [](StoryId) { return true; }, 15s);
        // Fence timeout 50 ms keeps the unconfirmed case fast.
        store_ = std::make_unique<InMemoryMetadataStore>(
                twoKeeperTopology(),
                [this](const KeeperRef& keeper, uint64_t revision)
                { return membership_->waitApplied(keeper.process_id, revision, 50ms); });
        pool_ = std::make_unique<WorkerPool>(2, 64);
        service_ = std::make_unique<CatalogService>(*store_, *pool_);

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_NE(port, 0);
        stub_ = v1::Catalog::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }

    void TearDown() override
    {
        if(server_)
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
    }

    template <class Rpc, class Request, class Response>
    grpc::Status call(Rpc rpc, const Request& request, Response* response)
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        return (stub_.get()->*rpc)(&context, request, response);
    }

    StoryId makeStory()
    {
        v1::CreateChronicleRequest chronicle;
        chronicle.set_name("c");
        v1::CreateChronicleResponse chronicle_response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, chronicle, &chronicle_response).ok());
        v1::CreateStoryRequest story;
        story.set_chronicle("c");
        story.set_name("s");
        v1::CreateStoryResponse story_response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::CreateStory, story, &story_response).ok());
        EXPECT_EQ(story_response.status().code(), 0);
        return story_response.story().story_id();
    }

    v1::AcquireResponse acquire(StoryId story, const std::string& identity)
    {
        v1::AcquireRequest request;
        request.set_story_id(story);
        request.set_writer_identity(identity);
        v1::AcquireResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, request, &response).ok());
        return response;
    }

    v1::ReleaseResponse release(const v1::AcquireResponse& acquired)
    {
        v1::ReleaseRequest request;
        request.set_story_id(acquired.story_id());
        request.set_writer_id(acquired.writer_id());
        request.set_incarnation(acquired.incarnation());
        v1::ReleaseResponse response;
        EXPECT_TRUE(call(&v1::Catalog::Stub::Release, request, &response).ok());
        return response;
    }

    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<InMemoryMetadataStore> store_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<CatalogService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<v1::Catalog::Stub> stub_;
};

TEST_F(catalog_adapter, AcquireCarriesAssignedKeeperEpochOneAndAStableWriterId)
{
    const StoryId story = makeStory();
    auto first = acquire(story, "w1");
    EXPECT_EQ(first.status().code(), 0);
    EXPECT_EQ(first.story_id(), story);
    EXPECT_EQ(first.writer_id(), 1u);
    EXPECT_EQ(first.incarnation(), 1u);
    EXPECT_EQ(first.route().epoch(), 1u);
    ASSERT_EQ(first.route().keepers_size(), 2);
    EXPECT_TRUE(first.assigned_keeper().process_id() == first.route().keepers(0).process_id() ||
                first.assigned_keeper().process_id() == first.route().keepers(1).process_id());
    EXPECT_FALSE(first.assigned_keeper().endpoint().empty());

    EXPECT_EQ(release(first).status().code(), 0);
    auto second = acquire(story, "w1");
    EXPECT_EQ(second.writer_id(), first.writer_id());
    EXPECT_EQ(second.incarnation(), 2u);
    EXPECT_EQ(second.assigned_keeper().process_id(), first.assigned_keeper().process_id());
    EXPECT_NE(acquire(story, "w2").writer_id(), first.writer_id());
}

TEST_F(catalog_adapter, MalformedRequestsFailTheWholeRequestWithInvalidArgument)
{
    v1::CreateChronicleResponse chronicle;
    EXPECT_EQ(call(&v1::Catalog::Stub::CreateChronicle, v1::CreateChronicleRequest(), &chronicle).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::GetStoryResponse story;
    EXPECT_EQ(call(&v1::Catalog::Stub::GetStory, v1::GetStoryRequest(), &story).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::AcquireRequest acquire_request;
    acquire_request.set_story_id(1);
    v1::AcquireResponse acquired;
    EXPECT_EQ(call(&v1::Catalog::Stub::Acquire, acquire_request, &acquired).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::ReleaseResponse released;
    EXPECT_EQ(call(&v1::Catalog::Stub::Release, v1::ReleaseRequest(), &released).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    v1::CompareAndSetEpochResponse epoch;
    EXPECT_EQ(call(&v1::Catalog::Stub::CompareAndSetEpoch, v1::CompareAndSetEpochRequest(), &epoch).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(catalog_adapter, WellFormedButStaleRequestsAreDomainResultsNotGrpcErrors)
{
    v1::AcquireRequest unknown;
    unknown.set_story_id(999);
    unknown.set_writer_identity("w");
    v1::AcquireResponse acquired;
    EXPECT_TRUE(call(&v1::Catalog::Stub::Acquire, unknown, &acquired).ok());
    EXPECT_EQ(acquired.status().code(), kNotFound);

    v1::GetChronicleRequest missing;
    missing.set_name("nope");
    v1::GetChronicleResponse got;
    EXPECT_TRUE(call(&v1::Catalog::Stub::GetChronicle, missing, &got).ok());
    EXPECT_EQ(got.status().code(), kNotFound);

    const StoryId story = makeStory();
    v1::CreateChronicleRequest again;
    again.set_name("c");
    v1::CreateChronicleResponse created;
    EXPECT_TRUE(call(&v1::Catalog::Stub::CreateChronicle, again, &created).ok());
    EXPECT_EQ(created.status().code(), kAlreadyExists);

    // I3.6: destroy while acquired is FAILED_PRECONDITION in the item status.
    auto held = acquire(story, "w1");
    v1::DestroyStoryRequest destroy;
    destroy.set_story_id(story);
    v1::DestroyStoryResponse status;
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyStory, destroy, &status).ok());
    EXPECT_EQ(status.status().code(), kFailedPrecondition);
    v1::DestroyChronicleRequest destroy_chronicle;
    destroy_chronicle.set_name("c");
    v1::DestroyChronicleResponse destroyed;
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyChronicle, destroy_chronicle, &destroyed).ok());
    EXPECT_EQ(destroyed.status().code(), kFailedPrecondition);

    // A second acquire with an active identity supersedes the old incarnation, and a
    // retried release of it returns the revision the supersede committed.
    auto newer = acquire(story, "w1");
    EXPECT_EQ(newer.status().code(), 0);
    EXPECT_GT(newer.incarnation(), held.incarnation());
    auto retry = release(held);
    EXPECT_EQ(retry.status().code(), 0);
    EXPECT_GT(retry.revision(), 0u);
    EXPECT_EQ(release(newer).status().code(), 0);
    EXPECT_TRUE(call(&v1::Catalog::Stub::DestroyStory, destroy, &status).ok());
    EXPECT_EQ(status.status().code(), 0);
}

TEST_F(catalog_adapter, ListsAndEpochCompareAndSet)
{
    const StoryId story = makeStory();
    v1::ListChroniclesResponse chronicles;
    EXPECT_TRUE(call(&v1::Catalog::Stub::ListChronicles, v1::ListChroniclesRequest(), &chronicles).ok());
    ASSERT_EQ(chronicles.chronicles_size(), 1);
    EXPECT_EQ(chronicles.chronicles(0).name(), "c");
    v1::ListStoriesRequest list;
    list.set_chronicle("c");
    v1::ListStoriesResponse stories;
    EXPECT_TRUE(call(&v1::Catalog::Stub::ListStories, list, &stories).ok());
    ASSERT_EQ(stories.stories_size(), 1);
    EXPECT_EQ(stories.stories(0).story_id(), story);
    EXPECT_EQ(stories.stories(0).epoch(), 1u);

    v1::CompareAndSetEpochRequest cas;
    cas.set_story_id(story);
    cas.set_expected(1);
    cas.set_desired(2);
    v1::CompareAndSetEpochResponse epoch;
    EXPECT_TRUE(call(&v1::Catalog::Stub::CompareAndSetEpoch, cas, &epoch).ok());
    EXPECT_EQ(epoch.status().code(), 0);
    EXPECT_EQ(epoch.epoch(), 2u);
    EXPECT_TRUE(call(&v1::Catalog::Stub::CompareAndSetEpoch, cas, &epoch).ok());
    EXPECT_EQ(epoch.status().code(), kFailedPrecondition);
}

TEST_F(catalog_adapter, ConcurrentAcquiresGetDistinctWriterIds)
{
    const StoryId story = makeStory();
    constexpr int kWriters = 16;
    std::vector<std::thread> threads;
    std::vector<uint64_t> ids(kWriters);
    for(int i = 0; i < kWriters; ++i)
    {
        threads.emplace_back(
                [&, i]
                {
                    v1::AcquireRequest request;
                    request.set_story_id(story);
                    request.set_writer_identity("writer-" + std::to_string(i));
                    v1::AcquireResponse response;
                    if(call(&v1::Catalog::Stub::Acquire, request, &response).ok() && response.status().code() == 0)
                        ids[i] = response.writer_id();
                });
    }
    for(auto& thread: threads) thread.join();
    std::set<uint64_t> distinct(ids.begin(), ids.end());
    EXPECT_EQ(distinct.size(), static_cast<size_t>(kWriters));
    EXPECT_EQ(distinct.count(0), 0u);
}

} // namespace
} // namespace chronolog::visor
