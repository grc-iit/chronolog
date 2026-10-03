#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include "rpc/Channel.h"
#include "chrono-player/adapter/ClusterClient.h"

namespace chronolog::player
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;
wire::RouteUpdate update(StoryId story = 42, Epoch epoch = 1, uint64_t revision = 1)
{
    wire::RouteUpdate r;
    r.set_story_id(story);
    r.set_revision(revision);
    r.mutable_route()->set_epoch(epoch);
    auto* k = r.mutable_route()->add_keepers();
    k->set_process_id("keeper-1");
    k->set_endpoint("keeper:50052");
    r.set_physical_policy(true);
    r.mutable_ordering_cut()->set_physical_ns(900);
    r.mutable_archived_below()->set_physical_ns(revision);
    auto* p = r.add_predecessors();
    p->mutable_keeper()->set_process_id("old");
    p->mutable_keeper()->set_endpoint("old:50052");
    p->set_instance("old-instance");
    p->set_epoch(1);
    p->mutable_own_cut()->set_physical_ns(200);
    p->set_own_physical_ceiling_ns(300);
    auto* lost = r.add_abandoned();
    lost->mutable_start()->set_physical_ns(120);
    lost->mutable_end()->set_physical_ns(150);
    return r;
}
class CatalogSnapshot final: public wire::Cluster::Service
{
public:
    std::mutex mu;
    std::condition_variable cv;
    std::vector<wire::RouteUpdate> snapshot{update()}, messages;
    std::atomic<int> calls{}, watches{}, heartbeats{};
    bool overflow{}, unknown{}, unavailable{};
    grpc::Status Register(grpc::ServerContext*, const wire::RegisterRequest*, wire::RegisterResponse* r) override
    {
        ++calls;
        std::lock_guard lock(mu);
        for(const auto& item: snapshot) *r->add_routes() = item;
        PhysicalPolicy expected;
        auto* p = r->mutable_policy();
        p->set_version(expected.version);
        p->set_skew_limit_ns(expected.skew_limit_ns);
        p->set_acceptance_window_ns(expected.acceptance_window_ns);
        p->set_hlc_lead_ns(expected.hlc_lead_ns);
        p->set_uncertainty_cap_ns(expected.uncertainty_cap_ns);
        unknown = false;
        cv.notify_all();
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatRequest*, wire::HeartbeatResponse* r) override
    {
        ++heartbeats;
        std::lock_guard lock(mu);
        cv.notify_all();
        if(unavailable)
            return {grpc::StatusCode::UNAVAILABLE, "unavailable"};
        if(unknown)
            r->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kNotFound));
        cv.notify_all();
        return grpc::Status::OK;
    }
    grpc::Status WatchRoutes(grpc::ServerContext* context,
                             const wire::WatchRoutesRequest*,
                             grpc::ServerWriter<wire::WatchRoutesResponse>* writer) override
    {
        std::unique_lock lock(mu);
        auto initial = snapshot;
        auto cursor = messages.size();
        ++watches;
        cv.notify_all();
        auto send = [&](const wire::RouteUpdate& item)
        {
            wire::WatchRoutesResponse response;
            response.ParseFromString(item.SerializeAsString());
            return writer->Write(response);
        };
        lock.unlock();
        for(const auto& item: initial)
            if(!send(item))
                return grpc::Status::OK;
        lock.lock();
        while(!context->IsCancelled())
        {
            if(overflow)
            {
                overflow = false;
                return {grpc::StatusCode::RESOURCE_EXHAUSTED, "resubscribe"};
            }
            if(cursor < messages.size())
            {
                const auto item = messages[cursor++];
                lock.unlock();
                const auto sent = send(item);
                lock.lock();
                if(!sent)
                    break;
            }
            else
                cv.wait_for(lock, 10ms);
        }
        return grpc::Status::OK;
    }
    void publish(wire::RouteUpdate item)
    {
        std::lock_guard lock(mu);
        std::erase_if(snapshot, [&](const auto& old) { return old.story_id() == item.story_id(); });
        if(!item.tombstoned())
            snapshot.push_back(item);
        messages.push_back(std::move(item));
        cv.notify_all();
    }
    bool subscribed(int count = 1)
    {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, 3s, [&] { return watches >= count; });
    }
};
class ClusterClientTest: public ::testing::Test
{
protected:
    CatalogSnapshot catalog;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ClusterClient> player;
    std::atomic<int> lookups{};
    std::atomic<bool> gone{}, fail_lookup{};
    void SetUp() override
    {
        grpc::ServerBuilder builder;
        int port = 0;
        rpc::applyServerPolicy(builder);
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&catalog);
        server = builder.BuildAndStart();
        ASSERT_NE(server, nullptr);
        channel = rpc::peerChannel("127.0.0.1:" + std::to_string(port));
        player = std::make_unique<ClusterClient>(channel,
                                                 Process{"player", "instance", "player:50054", ProcessRole::Player},
                                                 1s,
                                                 [this](StoryId id) -> absl::StatusOr<bool>
                                                 {
                                                     if(id == 99)
                                                         ++lookups;
                                                     if(fail_lookup)
                                                         return absl::UnavailableError("catalog unavailable");
                                                     if(id == 99)
                                                         return absl::NotFoundError("unknown story");
                                                     return gone.load();
                                                 });
    }
    void start()
    {
        ASSERT_TRUE(player->registerSelf().ok());
        ASSERT_TRUE(catalog.subscribed());
    }
    void TearDown() override
    {
        player.reset();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
    }
    absl::StatusOr<RouteState> after(StoryId id, Epoch epoch)
    {
        return player->routeStateAfter(id, epoch, std::chrono::system_clock::now() + 3s);
    }
};
TEST_F(ClusterClientTest, RouteStateDoesNotRegisterPerCall)
{
    start();
    for(int n = 0; n < 1000; ++n) ASSERT_TRUE(player->routeState(42).ok());
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, RouteStateFollowsWatchedEpochChange)
{
    start();
    catalog.publish(update(42, 2, 2));
    auto state = after(42, 1);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state->route.epoch, 2);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, TombstonedStoryIsReportedFromTheWatch)
{
    start();
    auto removed = update(42, 0, 0);
    removed.set_tombstoned(true);
    catalog.publish(removed);
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    catalog.publish(update(42, 9, 9));
    catalog.publish(update(43, 1, 10));
    ASSERT_TRUE(after(43, 0).ok());
    EXPECT_EQ(player->routeState(42).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, PreservesStateAndSameEpochPolicyChanges)
{
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(42, 2, 10), update(41, 1, 5)};
    }
    start();
    ASSERT_TRUE(player->route(41).ok());
    auto state = player->routeState(42);
    ASSERT_TRUE(state.ok());
    ASSERT_EQ(state->predecessors.size(), 1);
    EXPECT_EQ(state->predecessors[0].instance, "old-instance");
    EXPECT_EQ(state->predecessors[0].own_cut, (Hlc{200, 0}));
    EXPECT_EQ(state->predecessors[0].own_physical_ceiling_ns, 300);
    EXPECT_EQ(state->ordering_cut, (Hlc{900, 0}));
    ASSERT_EQ(state->abandoned.size(), 1);
    EXPECT_EQ(state->abandoned[0].start, (Hlc{120, 0}));
    EXPECT_TRUE(player->physicalPolicy(42));
    EXPECT_EQ(player->skewLimitNs(), 60000000000LL);
    auto changed = update(42, 2, 11);
    changed.set_physical_policy(false);
    catalog.publish(changed);
    catalog.publish(update(43, 1, 12));
    ASSERT_TRUE(after(43, 0).ok());
    EXPECT_FALSE(player->physicalPolicy(42));
    EXPECT_EQ(player->routeState(42)->archived_below, (Hlc{11, 0}));
    catalog.publish(update(42, 3, 9));
    catalog.publish(update(42, 1, 13));
    catalog.publish(update(44, 1, 14));
    ASSERT_TRUE(after(44, 0).ok());
    EXPECT_EQ(player->routeState(42)->route.epoch, 2);
    EXPECT_EQ(player->routeState(42)->archived_below, (Hlc{11, 0}));
}
TEST_F(ClusterClientTest, OverflowResubscribesAndReconcilesMissingTombstone)
{
    start();
    gone = true;
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 2, 2)};
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    auto state = after(43, 0);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state->route.epoch, 2);
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, UnseenStoryUsesOneLookupAndTransientFailureIsNotDeletion)
{
    start();
    const auto before = lookups.load();
    EXPECT_EQ(player->routeState(99).status().code(), absl::StatusCode::kNotFound);
    const auto once = lookups.load();
    EXPECT_GT(once, before);
    EXPECT_EQ(player->routeState(99).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ(lookups, once);
    fail_lookup = true;
    EXPECT_EQ(player->routeState(100).status().code(), absl::StatusCode::kUnavailable);
    fail_lookup = false;
    auto waiting = std::async(std::launch::async, [&] { return player->routeState(100); });
    catalog.publish(update(100, 1, 20));
    auto result = waiting.get();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, UnknownRegistrationReregistersButTransportFailureDoesNot)
{
    start();
    {
        std::lock_guard lock(catalog.mu);
        catalog.unknown = true;
    }
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.calls >= 2; }));
    }
    EXPECT_EQ(catalog.calls, 2);
    {
        std::lock_guard lock(catalog.mu);
        catalog.unavailable = true;
    }
    const int previous = catalog.heartbeats;
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.heartbeats > previous; }));
    }
    EXPECT_EQ(catalog.calls, 2);
}
} // namespace
} // namespace chronolog::player
