// ClusterService through a real in-process gRPC channel: registration, heartbeat,
// route snapshot and the acquisition stream that Keepers use to apply fences.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "catalog/AcquisitionFeed.h"
#include "catalog/InMemoryMetadataStore.h"
#include "catalog/SqliteMetadataStore.h"
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
                twoKeeperTopology(),
                1,
                [this](StoryId id)
                {
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

TEST_F(cluster_adapter, MismatchedPolicyRefusesRegistration)
{
    iv1::RegisterRequest request;
    auto* p = request.mutable_process();
    p->set_process_id("keeper-1");
    p->set_instance("instance");
    p->set_endpoint("localhost:1");
    p->set_role(iv1::PROCESS_ROLE_KEEPER);
    request.set_policy_version(2);
    iv1::RegisterResponse response;
    ASSERT_TRUE(stub_->Register(context().get(), request, &response).ok());
    EXPECT_EQ(response.status().code(), 9);
    EXPECT_FALSE(membership_->process("keeper-1"));
    request.set_policy_version(1);
    ASSERT_TRUE(stub_->Register(context().get(), request, &response).ok());
    EXPECT_EQ(response.status().code(), 0);
    EXPECT_EQ(response.policy().acceptance_window_ns(), 15000000000LL);
}

TEST_F(cluster_adapter, RegisterAndHeartbeatFenceAnObsoleteInstance)
{
    iv1::RegisterRequest registration;
    registration.mutable_process()->set_process_id("keeper-1");
    registration.mutable_process()->set_instance("i1");
    registration.mutable_process()->set_endpoint("keeper-a:50052");
    registration.mutable_process()->set_role(iv1::PROCESS_ROLE_KEEPER);
    iv1::RegisterResponse registered;
    ASSERT_TRUE(stub_->Register(context().get(), registration, &registered).ok());
    EXPECT_EQ(registered.status().code(), 0);
    EXPECT_GT(registered.authority_tick_ns(), 0u);
    // The Visor has no chrony bound, so it reports Unsynced without uncertainty.
    EXPECT_EQ(registered.physical().status(), v1::CLOCK_STATUS_UNSYNCED);
    EXPECT_FALSE(registered.physical().has_uncertainty_ns());

    iv1::HeartbeatRequest heartbeat;
    heartbeat.set_process_id("keeper-1");
    heartbeat.set_instance("i1");
    heartbeat.set_applied_revision(3);
    iv1::HeartbeatResponse answer;
    ASSERT_TRUE(stub_->Heartbeat(context().get(), heartbeat, &answer).ok());
    EXPECT_EQ(answer.status().code(), 0);
    EXPECT_TRUE(membership_->waitApplied("keeper-1", 3, 0ms));

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
    auto reader = stub_->WatchRoutes(ctx.get(), iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), story_);
    EXPECT_EQ(update.route().epoch(), 1u);
    EXPECT_EQ(update.route().keepers_size(), 2);
    // Server shutdown ends the held stream with OK.
    std::thread closer(
            [&]
            {
                std::this_thread::sleep_for(50ms);
                service_->shutdown();
            });
    EXPECT_FALSE(reader->Read(&update));
    closer.join();
    EXPECT_TRUE(reader->Finish().ok());
}

TEST_F(cluster_adapter, WatchAcquisitionsSendsAKeeperSnapshotThenLiveChangesInRevisionOrder)
{
    auto held = store_->acquire(story_, "before-subscribe");
    ASSERT_TRUE(held.ok());
    // A writer assigned to the other Keeper never reaches this stream.
    const KeeperRef other = held->assigned_keeper.process_id == "keeper-a" ? KeeperRef{"keeper-b", "keeper-b:50052"}
                                                                           : KeeperRef{"keeper-a", "keeper-a:50052"};
    auto ignored = store_->acquire(story_, "w-other");
    auto ignored_two = store_->acquire(story_, "w-other-2");
    ASSERT_TRUE(ignored.ok() && ignored_two.ok());

    auto ctx = context();
    iv1::WatchAcquisitionsRequest subscription;
    subscription.set_keeper_id(held->assigned_keeper.process_id);
    auto reader = stub_->WatchAcquisitions(ctx.get(), subscription);
    iv1::WatchAcquisitionsResponse message;
    ASSERT_TRUE(reader->Read(&message));
    ASSERT_TRUE(message.has_snapshot());
    const auto& snapshot = message.snapshot();
    EXPECT_GT(snapshot.revision(), 0u);
    ASSERT_GE(snapshot.acquisitions_size(), 1);
    bool found = false;
    for(const auto& item: snapshot.acquisitions())
    {
        EXPECT_EQ(item.assigned_keeper().process_id(), held->assigned_keeper.process_id);
        EXPECT_EQ(item.state(), iv1::ACQUISITION_STATE_ACQUIRED);
        found = found || item.writer_id() == held->writer_id;
    }
    EXPECT_TRUE(found);
    const uint64_t snapshot_revision = snapshot.revision();
    (void)other;

    // Identities hash to keepers by writer_id, so search for one on this Keeper.
    std::optional<Acquisition> live;
    for(int i = 0; i < 16 && !live; ++i)
    {
        auto candidate = store_->acquire(story_, "after-subscribe-" + std::to_string(i));
        ASSERT_TRUE(candidate.ok());
        if(candidate->assigned_keeper == held->assigned_keeper)
            live = *candidate;
    }
    ASSERT_TRUE(live.has_value());
    ASSERT_TRUE(reader->Read(&message));
    ASSERT_TRUE(message.has_update());
    EXPECT_EQ(message.update().writer_id(), live->writer_id);
    EXPECT_EQ(message.update().state(), iv1::ACQUISITION_STATE_ACQUIRED);
    EXPECT_GT(message.update().revision(), snapshot_revision);

    auto released = store_->release(story_, live->writer_id, live->incarnation);
    ASSERT_TRUE(released.ok());
    ASSERT_TRUE(reader->Read(&message));
    ASSERT_TRUE(message.has_update());
    EXPECT_EQ(message.update().writer_id(), live->writer_id);
    EXPECT_EQ(message.update().incarnation(), live->incarnation);
    EXPECT_EQ(message.update().state(), iv1::ACQUISITION_STATE_RELEASED);
    EXPECT_EQ(message.update().revision(), released->revision);

    ctx->TryCancel();
    while(reader->Read(&message)) {}
}

TEST_F(cluster_adapter, WatchAcquisitionsRequiresAKeeperId)
{
    auto ctx = context();
    auto reader = stub_->WatchAcquisitions(ctx.get(), iv1::WatchAcquisitionsRequest());
    iv1::WatchAcquisitionsResponse message;
    EXPECT_FALSE(reader->Read(&message));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(acquisition_feed, OverflowedSubscriberIsToldToResubscribe)
{
    AcquisitionFeed feed(2);
    InMemoryMetadataStore store(twoKeeperTopology());
    auto subscription = feed.subscribe(store);
    ASSERT_TRUE(subscription.ok());
    for(uint64_t i = 1; i <= 3; ++i)
        feed.onAcquisitionChange({i, 1, i, 1, KeeperRef{"k", "k:1"}, AcquisitionState::Acquired});
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
    const auto snapshot = (*subscription)->snapshot();
    ASSERT_EQ(snapshot.active.size(), 1u);
    EXPECT_EQ(snapshot.active[0].writer_id, early->writer_id);
    EXPECT_FALSE((*subscription)->pop().has_value());
    store.setObserver(nullptr);
}

TEST(acquisition_feed, SubscriptionSeesOnlyItsKeepersChanges)
{
    AcquisitionFeed feed;
    InMemoryMetadataStore store(twoKeeperTopology());
    auto subscription = feed.subscribe(store, "keeper-b");
    ASSERT_TRUE(subscription.ok());
    feed.onAcquisitionChange({1, 1, 1, 1, KeeperRef{"keeper-a", "keeper-a:1"}, AcquisitionState::Acquired});
    feed.onAcquisitionChange({2, 1, 2, 1, KeeperRef{"keeper-b", "keeper-b:1"}, AcquisitionState::Acquired});
    auto change = (*subscription)->pop();
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(change->revision, 2u);
    EXPECT_FALSE((*subscription)->pop().has_value());
}

// Static mode on the persisted Catalog: a destroy reaches WatchRoutes subscribers as tombstoned updates (W10.17).
class cluster_adapter_sqlite: public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto opened = SqliteMetadataStore::open((dir_.path() / "catalog").string(), twoKeeperTopology());
        ASSERT_TRUE(opened.ok());
        store_ = std::move(*opened);
        membership_ = std::make_unique<StaticRouteMembership>(
                twoKeeperTopology(),
                1,
                [this](StoryId id)
                {
                    auto story = store_->getStory(id);
                    return story.ok() && !story->tombstoned;
                },
                15s);
        feed_ = std::make_unique<AcquisitionFeed>();
        store_->setObserver(feed_.get());
        service_ = std::make_unique<ClusterService>(*membership_,
                                                    *store_,
                                                    *store_,
                                                    *feed_,
                                                    nullptr,
                                                    nullptr,
                                                    15s,
                                                    route_poll_period_);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = iv1::Cluster::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
        ASSERT_TRUE(store_->createChronicle("c").ok());
    }

    void TearDown() override
    {
        service_->shutdown();
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        store_->setObserver(nullptr);
    }

    std::chrono::milliseconds route_poll_period_{100};
    testing::TempDir dir_;
    std::unique_ptr<SqliteMetadataStore> store_;
    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<AcquisitionFeed> feed_;
    std::unique_ptr<ClusterService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<iv1::Cluster::Stub> stub_;
};

TEST_F(cluster_adapter_sqlite, WatchRoutesDeliversStoryAndChronicleDestroyAsTombstones)
{
    const auto kept = store_->createStory("c", "kept")->id;
    const auto gone = store_->createStory("c", "gone")->id;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reader = stub_->WatchRoutes(&ctx, iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    for(int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(reader->Read(&update));
        EXPECT_FALSE(update.tombstoned());
    }
    ASSERT_TRUE(store_->destroyStory(gone).ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), gone);
    EXPECT_TRUE(update.tombstoned());
    const auto first_revision = update.revision();
    EXPECT_GT(first_revision, 0u);

    // One chronicle destroy takes one revision for every story it destroys, and skips the story already gone.
    const auto also = store_->createStory("c", "also")->id;
    ASSERT_TRUE(store_->destroyChronicle("c").ok());
    std::set<StoryId> tombstoned;
    uint64_t revision = 0;
    while(tombstoned.size() < 2 && reader->Read(&update))
    {
        if(!update.tombstoned())
            continue;
        tombstoned.insert(update.story_id());
        if(revision == 0)
            revision = update.revision();
        EXPECT_EQ(update.revision(), revision);
    }
    EXPECT_EQ(tombstoned, (std::set<StoryId>{kept, also}));
    EXPECT_GT(revision, first_revision);
}

TEST_F(cluster_adapter_sqlite, WatchRoutesDeliversEveryRouteChangeInStaticMode)
{
    ASSERT_TRUE(store_->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store_->registerStaticPolicy("keeper-b", 1).ok());
    const auto first = store_->createStory("c", "first")->id;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reader = stub_->WatchRoutes(&ctx, iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), first);
    EXPECT_TRUE(update.physical_policy());
    const auto snapshot_revision = update.revision();

    const auto created = store_->createStory("c", "created")->id;
    ASSERT_TRUE(store_->clearPhysicalPolicy({first}).ok());
    ASSERT_TRUE(store_->registerStaticPolicy("keeper-a", 0).ok());
    ASSERT_TRUE(store_->destroyStory(created).ok());
    uint64_t revision = snapshot_revision;
    for(int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(reader->Read(&update));
        EXPECT_GT(update.revision(), revision);
        revision = update.revision();
        EXPECT_EQ(update.story_id(), i == 1 ? first : created);
        EXPECT_EQ(update.tombstoned(), i == 3);
        if(!update.tombstoned())
        {
            EXPECT_EQ(update.route().epoch(), 1u);
            EXPECT_EQ(update.physical_policy(), i == 0);
        }
    }
    ctx.TryCancel();
    (void)reader->Finish();
}

TEST_F(cluster_adapter_sqlite, WatchRoutesReconnectsWithoutDestroyedStoriesAfterHistoryTrim)
{
    const auto gone = store_->createStory("c", "gone")->id;
    const auto kept = store_->createStory("c", "kept")->id;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reader = stub_->WatchRoutes(&context, iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    for(int i = 0; i < 2; ++i) ASSERT_TRUE(reader->Read(&update));

    absl::Status mutation;
    auto applied = store_->applyRaft(1,
                                     [&]
                                     {
                                         mutation = store_->destroyStory(gone);
                                         if(!mutation.ok())
                                             return mutation.ToString();
                                         auto state = store_->membershipState();
                                         if(!state.ok())
                                         {
                                             mutation = state.status();
                                             return mutation.ToString();
                                         }
                                         state->set_route_history_floor(state->revision());
                                         mutation = store_->saveMembershipChanges(*state, *state);
                                         return mutation.ToString();
                                     });
    ASSERT_TRUE(applied.ok()) << applied.status();
    ASSERT_TRUE(mutation.ok()) << mutation;
    EXPECT_FALSE(reader->Read(&update));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);

    grpc::ClientContext resumed;
    resumed.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reconnect = stub_->WatchRoutes(&resumed, iv1::WatchRoutesRequest());
    ASSERT_TRUE(reconnect->Read(&update));
    EXPECT_EQ(update.story_id(), kept);
    EXPECT_FALSE(update.tombstoned());
    EXPECT_EQ(update.revision(), store_->membershipRevision().value_or(0));
    auto state = store_->membershipState();
    ASSERT_TRUE(state.ok());
    ASSERT_EQ(state->routes_size(), 1);
    EXPECT_EQ(state->routes(0).story_id(), kept);
    resumed.TryCancel();
    (void)reconnect->Finish();
}

TEST_F(cluster_adapter_sqlite, WatchRoutesReconnectOmitsStoryAndChronicleTombstones)
{
    const auto gone = store_->createStory("c", "gone")->id;
    const auto kept = store_->createStory("c", "kept")->id;
    ASSERT_TRUE(store_->createChronicle("removed").ok());
    ASSERT_TRUE(store_->createStory("removed", "first").ok());
    ASSERT_TRUE(store_->createStory("removed", "second").ok());
    grpc::ClientContext initial;
    initial.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reader = stub_->WatchRoutes(&initial, iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    for(int i = 0; i < 4; ++i) ASSERT_TRUE(reader->Read(&update));
    initial.TryCancel();
    (void)reader->Finish();
    ASSERT_TRUE(store_->destroyStory(gone).ok());
    ASSERT_TRUE(store_->destroyChronicle("removed").ok());

    grpc::ClientContext resumed;
    resumed.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reconnect = stub_->WatchRoutes(&resumed, iv1::WatchRoutesRequest());
    ASSERT_TRUE(reconnect->Read(&update));
    EXPECT_EQ(update.story_id(), kept);
    EXPECT_FALSE(update.tombstoned());
    EXPECT_EQ(update.revision(), store_->membershipRevision().value_or(0));
    resumed.TryCancel();
    (void)reconnect->Finish();
}

} // namespace
} // namespace chronolog::visor

namespace chronolog::visor
{
class cluster_route_wake: public cluster_adapter_sqlite
{
    void SetUp() override
    {
        route_poll_period_ = 1h;
        cluster_adapter_sqlite::SetUp();
    }
};
TEST_F(cluster_route_wake, RoutesArriveWithoutPeriodicTick)
{
    ASSERT_TRUE(store_->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store_->registerStaticPolicy("keeper-b", 1).ok());
    auto barrier = store_->createStory("c", "barrier");
    ASSERT_TRUE(barrier.ok());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 10s);
    auto reader = stub_->WatchRoutes(&context, iv1::WatchRoutesRequest());
    iv1::WatchRoutesResponse update;
    ASSERT_TRUE(reader->Read(&update));
    ASSERT_EQ(update.story_id(), barrier->id);
    auto revision = update.revision();
    auto created = store_->createStory("c", "created");
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), created->id);
    EXPECT_TRUE(update.physical_policy());
    EXPECT_GT(update.revision(), revision);
    revision = update.revision();
    ASSERT_TRUE(store_->clearPhysicalPolicy({created->id}).ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), created->id);
    EXPECT_FALSE(update.physical_policy());
    EXPECT_GT(update.revision(), revision);
    revision = update.revision();
    ASSERT_TRUE(store_->destroyStory(created->id).ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), created->id);
    EXPECT_TRUE(update.tombstoned());
    EXPECT_GT(update.revision(), revision);
    context.TryCancel();
    (void)reader->Finish();
}
} // namespace chronolog::visor
