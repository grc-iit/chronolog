#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include "rpc/Channel.h"
#include "chrono-player/adapter/ClusterClient.h"
#include "chrono-player/replay/HotReplay.h"
#include "chrono-visor/adapter/ClusterService.h"
#include "chrono-visor/adapter/WorkerPool.h"
#include "chrono-visor/catalog/SqliteMetadataStore.h"
#include "chrono-visor/tests/TestSupport.h"

namespace chronolog::player
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;
class CatalogSource final: public HotSource
{
public:
    explicit CatalogSource(std::shared_ptr<ClusterClient> routes)
        : routes_(std::move(routes))
    {}
    absl::StatusOr<HotFetch> fetch(StoryId story, const Range&) const override
    {
        auto route = routes_->routeState(story);
        if(!route.ok())
            return route.status();
        HotFetch result;
        result.route_epoch = route->route.epoch;
        result.physical_policy = routes_->physicalPolicy(story);
        for(const auto& keeper: route->route.keepers)
        {
            KeeperFetch fetched;
            fetched.frontier = {keeper.process_id, result.route_epoch, {500, 0}, true};
            fetched.frontier.physical_frontier = 500;
            result.keepers.push_back(fetched);
        }
        return result;
    }

private:
    std::shared_ptr<ClusterClient> routes_;
};
TEST(PhysicalPolicyCatalog, WatchReportsPolicyDowngradeBeforeLaterTombstone)
{
    visor::testing::TempDir dir;
    auto opened =
            visor::SqliteMetadataStore::open((dir.path() / "catalog").string(), visor::testing::twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    auto store = *std::move(opened);
    ASSERT_TRUE(store->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store->registerStaticPolicy("keeper-b", 1).ok());
    ASSERT_TRUE(store->createChronicle("c").ok());
    auto story = store->createStory("c", "s");
    auto barrier = store->createStory("c", "barrier");
    ASSERT_TRUE(story.ok());
    ASSERT_TRUE(barrier.ok());
    visor::StaticRouteMembership membership(visor::testing::twoKeeperTopology(), 1, [](StoryId) { return true; }, 15s);
    visor::AcquisitionFeed feed;
    visor::WorkerPool pool(2, 64);
    visor::ClusterService cluster(membership, *store, *store, feed, nullptr, &pool);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cluster);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = wire::Cluster::NewStub(rpc::peerChannel("127.0.0.1:" + std::to_string(port)));
    grpc::ClientContext context;
    rpc::withTimeout(context, 5s);
    wire::WatchRoutesRequest request;
    auto reader = stub->WatchRoutes(&context, request);
    wire::WatchRoutesResponse update;
    bool saw_story = false;
    for(int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(reader->Read(&update));
        if(update.story_id() == story->id)
        {
            saw_story = true;
            EXPECT_TRUE(update.physical_policy());
        }
    }
    ASSERT_TRUE(saw_story);
    ASSERT_TRUE(store->clearPhysicalPolicy({story->id}).ok());
    ASSERT_FALSE(store->membershipRouteUpdate(story->id)->physical_policy());
    ASSERT_TRUE(store->destroyStory(barrier->id).ok());
    bool saw_downgrade = false;
    bool saw_barrier = false;
    for(int i = 0; i < 8 && reader->Read(&update); ++i)
    {
        if(update.story_id() == story->id && !update.tombstoned() && !update.physical_policy())
            saw_downgrade = true;
        if(update.story_id() == barrier->id && update.tombstoned())
        {
            saw_barrier = true;
            break;
        }
    }
    context.TryCancel();
    (void)reader->Finish();
    EXPECT_TRUE(saw_barrier);
    EXPECT_TRUE(saw_downgrade) << "WatchRoutes omitted the physical-policy downgrade before the later destroy";
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}
TEST(PhysicalPolicyCatalog, UnmarkedArchiveHeartbeatPermanentlyPreventsPhysicalCompletion)
{
    visor::testing::TempDir dir;
    auto opened =
            visor::SqliteMetadataStore::open((dir.path() / "catalog").string(), visor::testing::twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    auto store = *std::move(opened);
    ASSERT_TRUE(store->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store->registerStaticPolicy("keeper-b", 1).ok());
    ASSERT_TRUE(store->createChronicle("c").ok());
    auto story = store->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    ASSERT_TRUE(store->membershipRouteUpdate(story->id)->physical_policy());
    visor::StaticRouteMembership membership(visor::testing::twoKeeperTopology(), 1, [](StoryId) { return true; }, 15s);
    visor::AcquisitionFeed feed;
    visor::WorkerPool pool(2, 64);
    visor::ClusterService cluster(membership, *store, *store, feed, nullptr, &pool);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cluster);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    std::mutex policy_mutex;
    std::condition_variable policy_changed;
    auto routes = std::make_shared<ClusterClient>(channel,
                                                  Process{"player", "instance", "player:50054", ProcessRole::Player},
                                                  2s);
    routes->onRoute(
            [&](const Route&)
            {
                std::lock_guard lock(policy_mutex);
                policy_changed.notify_all();
            });
    auto source = std::make_shared<CatalogSource>(routes);
    auto complete = [&]()
    {
        HotReplay replay(source);
        auto stream = replay.read(story->id, {Range::Axis::Physical, {150, 0}, {151, 0}});
        EXPECT_TRUE(stream.ok());
        if(!stream.ok())
            return true;
        auto final = (*stream)->next();
        EXPECT_TRUE(final.ok());
        return final.ok() && *final && (**final).completion && (**final).completion->complete;
    };
    EXPECT_TRUE(complete());
    auto archive = FileTierStore::Open(dir.path() / "archive", "grapher", {{story->id, {100, 0}}});
    ASSERT_TRUE(archive.ok());
    Event event;
    event.id = {story->id, 2, 1, 1};
    event.hlc = {150, 0};
    event.physical = {150, 0, ClockStatus::Synced};
    ASSERT_TRUE((*archive)->publish({"legacy", story->id, {100, 0}, {200, 0}, {event}, false}).ok());
    auto legacy = (*archive)->storiesWithoutPhysicalPolicy();
    ASSERT_TRUE(legacy.ok());
    ASSERT_EQ(*legacy, (std::vector<StoryId>{story->id}));
    auto stub = wire::Cluster::NewStub(channel);
    wire::RegisterRequest registration;
    auto* p = registration.mutable_process();
    p->set_process_id("grapher");
    p->set_instance("grapher-instance");
    p->set_role(wire::PROCESS_ROLE_GRAPHER);
    p->set_endpoint("grapher:50053");
    wire::RegisterResponse registered;
    grpc::ClientContext registering;
    registering.set_deadline(std::chrono::system_clock::now() + 2s);
    ASSERT_TRUE(stub->Register(&registering, registration, &registered).ok());
    ASSERT_EQ(registered.status().code(), 0);
    wire::HeartbeatRequest heartbeat;
    heartbeat.set_process_id("grapher");
    heartbeat.set_instance("grapher-instance");
    for(const auto id: *legacy) heartbeat.add_stories_without_physical_policy(id);
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        wire::HeartbeatResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 2s);
        ASSERT_TRUE(stub->Heartbeat(&context, heartbeat, &response).ok());
        EXPECT_EQ(response.status().code(), 0);
    }
    EXPECT_FALSE(store->membershipRouteUpdate(story->id)->physical_policy());
    {
        std::unique_lock lock(policy_mutex);
        ASSERT_TRUE(policy_changed.wait_for(lock, 2s, [&] { return !routes->physicalPolicy(story->id); }));
    }
    EXPECT_FALSE(complete());
    ASSERT_TRUE(store->registerStaticPolicy("keeper-a", 1).ok());
    EXPECT_FALSE(complete());
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}
} // namespace
} // namespace chronolog::player
