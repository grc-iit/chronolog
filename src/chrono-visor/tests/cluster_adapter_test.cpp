// ClusterService through a real in-process gRPC channel: registration, heartbeat,
// route snapshot and the acquisition stream that Keepers use to apply fences.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "catalog/AcquisitionFeed.h"
#include "catalog/InMemoryMetadataStore.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{
namespace
{

using namespace std::chrono_literals;
using testing::twoKeeperTopology;
namespace iv1 = chronolog::internal::v1;

class cluster_adapter: public ::testing::Test
{
protected:
    void SetUp() override
    {
        store_ = std::make_unique<InMemoryMetadataStore>(twoKeeperTopology());
        membership_ = std::make_unique<StaticRouteMembership>(
                twoKeeperTopology(), 1,
                [this](StoryId id) {
                    auto story = store_->getStory(id);
                    return story.ok() && !story->tombstoned;
                },
                15s);
        feed_ = std::make_unique<AcquisitionFeed>();
        store_->setObserver(feed_.get());
        service_ = std::make_unique<ClusterService>(*membership_, *store_, *store_, *feed_);

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = iv1::Cluster::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
        ASSERT_TRUE(store_->createChronicle("c").ok());
        story_ = store_->createStory("c", "s")->id;
    }

    void TearDown() override
    {
        service_->shutdown();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        store_->setObserver(nullptr);
    }

    std::unique_ptr<grpc::ClientContext> context()
    {
        auto ctx = std::make_unique<grpc::ClientContext>();
        ctx->set_deadline(std::chrono::system_clock::now() + 10s);
        return ctx;
    }

    std::unique_ptr<InMemoryMetadataStore> store_;
    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<AcquisitionFeed> feed_;
    std::unique_ptr<ClusterService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<iv1::Cluster::Stub> stub_;
    StoryId story_{};
};

TEST_F(cluster_adapter, RegisterAndHeartbeatFenceAnObsoleteInstance)
{
    iv1::RegisterRequest registration;
    registration.mutable_process()->set_process_id("keeper-1");
    registration.mutable_process()->set_instance("i1");
    registration.mutable_process()->set_endpoint("keeper-a:50052");
    registration.mutable_process()->set_role(iv1::KEEPER);
    iv1::RegisterResponse registered;
    ASSERT_TRUE(stub_->Register(context().get(), registration, &registered).ok());
    EXPECT_EQ(registered.status().code(), 0);
    EXPECT_GT(registered.authority_tick_ns(), 0u);
    // The Visor has no chrony bound, so it reports Unsynced without uncertainty.
    EXPECT_EQ(registered.physical().status(), v1::UNSYNCED);
    EXPECT_FALSE(registered.physical().has_uncertainty_ns());

    iv1::HeartbeatRequest heartbeat;
    heartbeat.set_process_id("keeper-1");
    heartbeat.set_instance("i1");
    heartbeat.set_applied_revision(3);
    iv1::HeartbeatResponse answer;
    ASSERT_TRUE(stub_->Heartbeat(context().get(), heartbeat, &answer).ok());
    EXPECT_EQ(answer.status().code(), 0);
    EXPECT_TRUE(membership_->waitApplied("keeper-a:50052", 3, 0ms));

    registration.mutable_process()->set_instance("i2");
    ASSERT_TRUE(stub_->Register(context().get(), registration, &registered).ok());
    ASSERT_TRUE(stub_->Heartbeat(context().get(), heartbeat, &answer).ok());
    EXPECT_EQ(answer.status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
}

TEST_F(cluster_adapter, RegisterWithoutARoleIsAnItemFailure)
{
    iv1::RegisterRequest registration;
    registration.mutable_process()->set_process_id("keeper-1");
    registration.mutable_process()->set_instance("i1");
    registration.mutable_process()->set_endpoint("keeper-a:50052");
    iv1::RegisterResponse registered;
    ASSERT_TRUE(stub_->Register(context().get(), registration, &registered).ok());
    EXPECT_EQ(registered.status().code(), static_cast<int>(absl::StatusCode::kInvalidArgument));
}

TEST_F(cluster_adapter, ReadClockIsUnimplemented)
{
    iv1::ReadClockResponse response;
    EXPECT_EQ(stub_->ReadClock(context().get(), iv1::ReadClockRequest(), &response).error_code(),
              grpc::StatusCode::UNIMPLEMENTED);
}

TEST_F(cluster_adapter, WatchRoutesSendsOneFullSnapshotThenHolds)
{
    auto ctx = context();
    auto reader = stub_->WatchRoutes(ctx.get(), iv1::RouteSubscription());
    iv1::RouteUpdate update;
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), story_);
    EXPECT_EQ(update.route().epoch(), 1u);
    EXPECT_EQ(update.route().keepers_size(), 2);
    // Server shutdown ends the held stream with OK.
    std::thread closer([&] {
        std::this_thread::sleep_for(50ms);
        service_->shutdown();
    });
    EXPECT_FALSE(reader->Read(&update));
    closer.join();
    EXPECT_TRUE(reader->Finish().ok());
}

TEST_F(cluster_adapter, WatchAcquisitionsSendsSnapshotThenLiveChangesInRevisionOrder)
{
    auto held = store_->acquire(story_, "before-subscribe");
    ASSERT_TRUE(held.ok());

    auto ctx = context();
    auto reader = stub_->WatchAcquisitions(ctx.get(), iv1::AcquisitionSubscription());
    iv1::AcquisitionUpdate update;
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.writer_id(), held->writer_id);
    EXPECT_EQ(update.state(), iv1::ACQUIRED);
    EXPECT_EQ(update.assigned_keeper(), held->assigned_keeper);
    const uint64_t snapshot_revision = update.revision();
    EXPECT_GT(snapshot_revision, 0u);

    auto live = store_->acquire(story_, "after-subscribe");
    ASSERT_TRUE(live.ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.writer_id(), live->writer_id);
    EXPECT_EQ(update.state(), iv1::ACQUIRED);
    EXPECT_GT(update.revision(), snapshot_revision);

    ASSERT_TRUE(store_->release(story_, live->writer_id, live->incarnation).ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.writer_id(), live->writer_id);
    EXPECT_EQ(update.incarnation(), live->incarnation);
    EXPECT_EQ(update.state(), iv1::RELEASED);

    ctx->TryCancel();
    while(reader->Read(&update))
    {}
}

TEST(acquisition_feed, OverflowedSubscriberIsToldToResubscribe)
{
    AcquisitionFeed feed(2);
    InMemoryMetadataStore store(twoKeeperTopology());
    auto subscription = feed.subscribe(store);
    ASSERT_TRUE(subscription.ok());
    for(uint64_t i = 1; i <= 3; ++i)
        feed.onAcquisitionChange({i, 1, i, 1, "k", AcquisitionState::Acquired});
    EXPECT_TRUE((*subscription)->overflowed());
    EXPECT_FALSE((*subscription)->pop().has_value());
}

TEST(acquisition_feed, ChangesCoveredByTheSnapshotAreDropped)
{
    AcquisitionFeed feed;
    InMemoryMetadataStore store(twoKeeperTopology());
    ASSERT_TRUE(store.createChronicle("c").ok());
    const StoryId story = store.createStory("c", "s")->id;
    store.setObserver(&feed);
    // A change committed before the subscription is in the snapshot and must not
    // also arrive from the queue.
    auto early = store.acquire(story, "w");
    ASSERT_TRUE(early.ok());
    auto subscription = feed.subscribe(store);
    ASSERT_TRUE(subscription.ok());
    auto first = (*subscription)->pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->writer_id, early->writer_id);
    EXPECT_FALSE((*subscription)->pop().has_value());
    store.setObserver(nullptr);
}

} // namespace
} // namespace chronolog::visor
