#include <gtest/gtest.h>
#include <atomic>
#include <future>

#include "visor/tests/TestSupport.h"
#include "visor/adapter/CatalogService.h"
#include "visor/adapter/ClusterService.h"
#include "visor/catalog/SqliteMetadataStore.h"

namespace chronolog::visor
{
namespace
{
using namespace std::chrono_literals;
namespace iv1 = internal::v1;
struct CatalogRig
{
    testing::TempDir directory;
    Topology topology{{{"keeper", "keeper:1"}}, "grapher:1", "player:1"};
    StaticRouteMembership membership{topology, 1, [](StoryId id) { return id == 1; }, 15s};
    std::function<void(uint64_t)> waiting;
    std::unique_ptr<SqliteMetadataStore> store;
    AcquisitionFeed feed;
    WorkerPool pool{2, 32};
    std::unique_ptr<CatalogService> catalog;
    std::unique_ptr<ClusterService> cluster;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<v1::Catalog::Stub> stub;
    std::unique_ptr<iv1::Cluster::Stub> cluster_stub;
    explicit CatalogRig(std::function<void(uint64_t)> on_wait, std::chrono::milliseconds timeout = 500ms)
        : waiting(std::move(on_wait))
    {
        auto opened = SqliteMetadataStore::open((directory.path() / "catalog.sqlite").string(),
                                                topology,
                                                [this, timeout](const KeeperRef& keeper, uint64_t revision)
                                                {
                                                    if(waiting)
                                                        waiting(revision);
                                                    return membership.waitApplied(keeper.process_id, revision, timeout);
                                                });
        if(!opened.ok())
            throw std::runtime_error(std::string(opened.status().message()));
        store = std::move(*opened);
        store->setObserver(&feed);
        catalog = std::make_unique<CatalogService>(*store, pool);
        cluster = std::make_unique<ClusterService>(membership, *store, *store, feed);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(catalog.get());
        builder.RegisterService(cluster.get());
        server = builder.BuildAndStart();
        if(!server)
            throw std::runtime_error("Catalog server did not start");
        auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub = v1::Catalog::NewStub(channel);
        cluster_stub = iv1::Cluster::NewStub(channel);
    }
    ~CatalogRig()
    {
        cluster->shutdown();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
        store->setObserver(nullptr);
    }
    void heartbeat(uint64_t revision)
    {
        iv1::HeartbeatRequest request;
        request.set_process_id("keeper");
        request.set_instance("instance");
        request.set_applied_revision(revision);
        iv1::HeartbeatResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        EXPECT_EQ(cluster_stub->Heartbeat(&context, request, &response).error_code(), grpc::StatusCode::OK);
        EXPECT_EQ(response.status().code(), 0);
    }
    void registerKeeper()
    {
        iv1::RegisterRequest request;
        auto* process = request.mutable_process();
        process->set_process_id("keeper");
        process->set_instance("instance");
        process->set_endpoint("keeper:1");
        process->set_role(iv1::PROCESS_ROLE_KEEPER);
        iv1::RegisterResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        EXPECT_EQ(cluster_stub->Register(&context, request, &response).error_code(), grpc::StatusCode::OK);
        EXPECT_EQ(response.status().code(), 0);
    }
};
} // namespace

TEST(CatalogAdapterTest, ReleaseWaitsForAppliedRevision)
{
    std::promise<uint64_t> waiting;
    CatalogRig rig([&](uint64_t revision) { waiting.set_value(revision); }, 5s);
    ASSERT_EQ(rig.store->createChronicle("c").status().code(), absl::StatusCode::kOk);
    auto story = rig.store->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    auto acquired = rig.store->acquire(story->id, "writer");
    ASSERT_TRUE(acquired.ok());
    rig.registerKeeper();
    v1::ReleaseRequest request;
    request.set_story_id(story->id);
    request.set_writer_id(acquired->writer_id);
    request.set_incarnation(acquired->incarnation);
    auto released = std::async(std::launch::async,
                               [&]
                               {
                                   grpc::ClientContext context;
                                   context.set_deadline(std::chrono::system_clock::now() + 5s);
                                   v1::ReleaseResponse response;
                                   EXPECT_EQ(rig.stub->Release(&context, request, &response).error_code(),
                                             grpc::StatusCode::OK);
                                   return response;
                               });
    auto committed = waiting.get_future();
    ASSERT_EQ(committed.wait_for(5s), std::future_status::ready);
    const auto revision = committed.get();
    EXPECT_GT(revision, 0u);
    rig.heartbeat(revision - 1);
    EXPECT_EQ(released.wait_for(20ms), std::future_status::timeout);
    rig.heartbeat(revision);
    ASSERT_EQ(released.wait_for(5s), std::future_status::ready);
    auto response = released.get();
    EXPECT_EQ(response.status().code(), 0);
    EXPECT_TRUE(response.fenced());
    EXPECT_EQ(response.revision(), revision);
}

TEST(CatalogAdapterTest, DroppedReleaseRetryReturnsCommittedState)
{
    std::promise<uint64_t> waiting;
    std::atomic<bool> first{true};
    CatalogRig rig(
            [&](uint64_t revision)
            {
                if(first.exchange(false))
                    waiting.set_value(revision);
            });
    ASSERT_TRUE(rig.store->createChronicle("c").ok());
    auto story = rig.store->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    auto acquired = rig.store->acquire(story->id, "writer");
    ASSERT_TRUE(acquired.ok());
    rig.registerKeeper();
    v1::ReleaseRequest request;
    request.set_story_id(story->id);
    request.set_writer_id(acquired->writer_id);
    request.set_incarnation(acquired->incarnation);
    grpc::ClientContext lost;
    lost.set_deadline(std::chrono::system_clock::now() + 5s);
    auto call = std::async(std::launch::async,
                           [&]
                           {
                               v1::ReleaseResponse response;
                               return rig.stub->Release(&lost, request, &response);
                           });
    auto committed = waiting.get_future();
    ASSERT_EQ(committed.wait_for(5s), std::future_status::ready);
    const auto revision = committed.get();
    lost.TryCancel();
    EXPECT_EQ(call.get().error_code(), grpc::StatusCode::CANCELLED);
    auto snapshot = rig.store->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    EXPECT_TRUE(snapshot->active.empty());
    EXPECT_EQ(snapshot->revision, revision);
    grpc::ClientContext retry;
    retry.set_deadline(std::chrono::system_clock::now() + 5s);
    v1::ReleaseResponse unconfirmed;
    ASSERT_EQ(rig.stub->Release(&retry, request, &unconfirmed).error_code(), grpc::StatusCode::OK);
    EXPECT_EQ(unconfirmed.status().code(), 0);
    EXPECT_FALSE(unconfirmed.fenced());
    EXPECT_EQ(unconfirmed.revision(), revision);
    rig.heartbeat(revision);
    grpc::ClientContext confirmed;
    confirmed.set_deadline(std::chrono::system_clock::now() + 5s);
    v1::ReleaseResponse response;
    ASSERT_EQ(rig.stub->Release(&confirmed, request, &response).error_code(), grpc::StatusCode::OK);
    EXPECT_EQ(response.status().code(), 0);
    EXPECT_TRUE(response.fenced());
    EXPECT_EQ(response.revision(), revision);
    snapshot = rig.store->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot->revision, revision);
}
} // namespace chronolog::visor
