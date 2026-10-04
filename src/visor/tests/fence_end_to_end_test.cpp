// Release fence through real gRPC: a fake Keeper subscribes to WatchAcquisitions and
// heartbeats the revisions it applied. Release reports fenced=true while the Keeper
// runs and fenced=false after the injected timeout once it stops.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "visor/tests/TestSupport.h"
#include "visor/adapter/CatalogService.h"
#include "visor/adapter/ClusterService.h"
#include "common/worker/WorkerPool.h"
#include "visor/catalog/AcquisitionFeed.h"
#include "visor/catalog/InMemoryMetadataStore.h"
#include "visor/catalog/LeaseAuthority.h"
#include "visor/membership/StaticRouteMembership.h"

namespace chronolog::visor
{
namespace
{

using namespace std::chrono_literals;
using testing::twoKeeperTopology;
namespace iv1 = chronolog::internal::v1;

class FakeKeeper
{
public:
    FakeKeeper(iv1::Cluster::Stub& stub, std::string id)
        : stub_(stub)
        , id_(std::move(id))
    {
        iv1::RegisterRequest registration;
        registration.mutable_process()->set_process_id(id_);
        registration.mutable_process()->set_instance("fake-1");
        registration.mutable_process()->set_endpoint(id_ + ":50052");
        registration.mutable_process()->set_role(iv1::PROCESS_ROLE_KEEPER);
        iv1::RegisterResponse registered;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        registered_ = stub_.Register(&context, registration, &registered).ok() && registered.status().code() == 0;
        thread_ = std::thread([this] { run(); });
    }

    ~FakeKeeper() { stop(); }

    bool registered() const { return registered_; }

    void stop()
    {
        stopping_ = true;
        {
            std::lock_guard lock(mutex_);
            if(context_)
                context_->TryCancel();
        }
        if(thread_.joinable())
            thread_.join();
    }

private:
    void run()
    {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 30s);
        {
            std::lock_guard lock(mutex_);
            context_ = &context;
            if(stopping_)
                context.TryCancel();
        }
        iv1::WatchAcquisitionsRequest subscription;
        subscription.set_keeper_id(id_);
        auto reader = stub_.WatchAcquisitions(&context, subscription);
        iv1::WatchAcquisitionsResponse message;
        uint64_t applied = 0;
        while(!stopping_ && reader->Read(&message))
        {
            applied = std::max(applied,
                               message.has_snapshot() ? message.snapshot().revision() : message.update().revision());
            if(stopping_)
                break;
            iv1::HeartbeatRequest heartbeat;
            heartbeat.set_process_id(id_);
            heartbeat.set_instance("fake-1");
            heartbeat.set_applied_revision(applied);
            iv1::HeartbeatResponse answer;
            grpc::ClientContext call;
            call.set_deadline(std::chrono::system_clock::now() + 10s);
            stub_.Heartbeat(&call, heartbeat, &answer);
        }
        {
            std::lock_guard lock(mutex_);
            context_ = nullptr;
        }
        context.TryCancel();
        while(reader->Read(&message)) {}
    }

    iv1::Cluster::Stub& stub_;
    const std::string id_;
    std::mutex mutex_;
    grpc::ClientContext* context_{};
    std::atomic<bool> stopping_{false};
    bool registered_{};
    std::thread thread_;
};

class fence_end_to_end: public ::testing::Test
{
protected:
    void SetUp() override
    {
        membership_ =
                std::make_unique<StaticRouteMembership>(twoKeeperTopology(), 1, [](StoryId) { return true; }, 15s);
        store_ = std::make_unique<InMemoryMetadataStore>(
                twoKeeperTopology(),
                [this](const KeeperRef& keeper, uint64_t revision)
                { return membership_->waitApplied(keeper.process_id, revision, 300ms); });
        feed_ = std::make_unique<AcquisitionFeed>();
        store_->setObserver(feed_.get());
        pool_ = std::make_unique<WorkerPool>(2, 64);
        catalog_ = std::make_unique<CatalogService>(*store_, *pool_);
        cluster_ = std::make_unique<ClusterService>(*membership_, *store_, *store_, *feed_);

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(catalog_.get());
        builder.RegisterService(cluster_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        catalog_stub_ = v1::Catalog::NewStub(channel);
        cluster_stub_ = iv1::Cluster::NewStub(channel);
    }

    void TearDown() override
    {
        keeper_.reset();
        cluster_->shutdown();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        store_->setObserver(nullptr);
    }

    v1::AcquireResponse acquire(StoryId story)
    {
        v1::AcquireRequest request;
        request.set_story_id(story);
        request.set_writer_identity("writer");
        request.set_acquire_request_id(newAcquireRequestId());
        v1::AcquireResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        EXPECT_TRUE(catalog_stub_->Acquire(&context, request, &response).ok());
        return response;
    }

    v1::ReleaseResponse release(const v1::AcquireResponse& acquired)
    {
        v1::ReleaseRequest request;
        request.set_story_id(acquired.story_id());
        request.set_writer_id(acquired.writer_id());
        request.set_incarnation(acquired.incarnation());
        v1::ReleaseResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        EXPECT_TRUE(catalog_stub_->Release(&context, request, &response).ok());
        return response;
    }

    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<InMemoryMetadataStore> store_;
    std::unique_ptr<AcquisitionFeed> feed_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<CatalogService> catalog_;
    std::unique_ptr<ClusterService> cluster_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<v1::Catalog::Stub> catalog_stub_;
    std::unique_ptr<iv1::Cluster::Stub> cluster_stub_;
    std::unique_ptr<FakeKeeper> keeper_;
};

TEST_F(fence_end_to_end, ReleaseIsFencedWhileTheKeeperAppliesAndUnfencedAfterItStops)
{
    ASSERT_TRUE(store_->createChronicle("c").ok());
    const StoryId story = store_->createStory("c", "s")->id;

    auto first = acquire(story);
    ASSERT_EQ(first.status().code(), 0);
    keeper_ = std::make_unique<FakeKeeper>(*cluster_stub_, first.assigned_keeper().process_id());
    ASSERT_TRUE(keeper_->registered());

    auto fenced = release(first);
    EXPECT_EQ(fenced.status().code(), 0);
    EXPECT_TRUE(fenced.fenced());
    EXPECT_GT(fenced.revision(), 0u);

    keeper_->stop();
    auto second = acquire(story);
    ASSERT_EQ(second.status().code(), 0);
    EXPECT_EQ(second.assigned_keeper().process_id(), first.assigned_keeper().process_id());
    const auto started = std::chrono::steady_clock::now();
    auto unfenced = release(second);
    EXPECT_EQ(unfenced.status().code(), 0);
    EXPECT_FALSE(unfenced.fenced());
    EXPECT_GT(unfenced.revision(), fenced.revision());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
}

} // namespace
} // namespace chronolog::visor
