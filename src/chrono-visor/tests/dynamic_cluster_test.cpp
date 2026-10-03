#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "adapter/WorkerPool.h"
#include "dynamic/MembershipState.h"
#include "raft/RaftMetadataStore.h"
#include "clock/FakeClock.h"
#include "membership/ConfigMembership.h"
#include "rpc/Channel.h"
#include "runtime/ClusterClient.h"
#include <gtest/gtest.h>
#include <array>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <string_view>
#include <random>
#include <set>
namespace chronolog::visor
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;
int freePort()
{
    static std::mt19937 random(std::random_device{}());
    static std::set<int> chosen;
    for(int attempt = 0; attempt < 64; ++attempt)
    {
        const int port = 10000 + (random() % 4400) * 5;
        if(chosen.contains(port))
            continue;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0)
            throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        const int result = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        close(fd);
        if(result == 0)
        {
            chosen.insert(port);
            return port;
        }
    }
    throw std::runtime_error("no free loopback port block");
}
struct KeeperDriver
{
    wire::Cluster::Stub& stub;
    std::string id, instance;
    uint64_t revision = 0;
    std::unique_ptr<grpc::ClientContext> context()
    {
        auto c = std::make_unique<grpc::ClientContext>();
        c->set_deadline(std::chrono::system_clock::now() + 4s);
        return c;
    }
    wire::RegisterResponse Register()
    {
        wire::RegisterRequest q;
        auto* p = q.mutable_process();
        p->set_process_id(id);
        p->set_instance(instance);
        p->set_endpoint(id + ":50052");
        p->set_role(wire::PROCESS_ROLE_KEEPER);
        wire::RegisterResponse r;
        auto status = stub.Register(context().get(), q, &r);
        if(!status.ok())
            throw std::runtime_error(status.error_message());
        for(const auto& route: r.routes()) revision = std::max(revision, route.revision());
        return r;
    }
    wire::HeartbeatResponse Heartbeat()
    {
        wire::HeartbeatRequest q;
        q.set_process_id(id);
        q.set_instance(instance);
        q.set_applied_route_revision(revision);
        q.set_applied_revision(revision);
        wire::HeartbeatResponse r;
        auto status = stub.Heartbeat(context().get(), q, &r);
        if(!status.ok())
            throw std::runtime_error(status.error_message());
        return r;
    }
    wire::ExtendCeilingResponse ExtendCeiling()
    {
        wire::ExtendCeilingRequest q;
        q.set_process_id(id);
        q.set_instance(instance);
        q.set_applied_route_revision(revision);
        q.set_realtime_ns(100);
        q.mutable_wanted_hlc()->set_physical_ns(100);
        wire::ExtendCeilingResponse r;
        auto status = stub.ExtendCeiling(context().get(), q, &r);
        if(!status.ok())
            throw std::runtime_error(status.error_message());
        for(const auto& route: r.routes()) revision = std::max(revision, route.revision());
        return r;
    }
};
class DynamicClusterTest: public ::testing::Test
{
protected:
    testing::TempDir dir;
    std::array<std::unique_ptr<RaftMetadataStore>, 3> stores;
    std::array<std::unique_ptr<StaticRouteMembership>, 3> memberships;
    std::array<std::unique_ptr<AcquisitionFeed>, 3> feeds;
    std::array<std::unique_ptr<WorkerPool>, 3> pools;
    std::array<std::unique_ptr<ClusterService>, 3> services;
    std::array<std::unique_ptr<grpc::Server>, 3> servers;
    std::array<std::unique_ptr<wire::Cluster::Stub>, 3> stubs;
    std::array<std::shared_ptr<grpc::Channel>, 3> channels;
    absl::Status start(int attempt)
    {
        std::filesystem::create_directory(dir.path() / std::to_string(attempt));
        std::vector<RaftPeer> peers;
        for(int i = 0; i < 3; ++i)
        {
            auto raft_endpoint = "127.0.0.1:" + std::to_string(freePort());
            auto endpoint = "127.0.0.1:" + std::to_string(freePort());
            peers.push_back({i + 1, raft_endpoint, endpoint, endpoint});
        }
        for(size_t i = 0; i < 3; ++i)
        {
            auto opened = RaftMetadataStore::open((dir.path() / std::to_string(attempt) / std::to_string(i)).string(),
                                                  testing::twoKeeperTopology(),
                                                  {static_cast<int32_t>(i + 1), peers[i].raft_endpoint, peers});
            if(!opened.ok())
                return opened.status();
            stores[i] = std::move(*opened);
            memberships[i] = std::make_unique<StaticRouteMembership>(
                    testing::twoKeeperTopology(),
                    1,
                    [this, i](StoryId id) { return stores[i]->appliedStore().getStory(id).ok(); },
                    1500ms);
            feeds[i] = std::make_unique<AcquisitionFeed>();
            pools[i] = std::make_unique<WorkerPool>(2, 64);
            services[i] = std::make_unique<ClusterService>(*memberships[i],
                                                           stores[i]->appliedStore(),
                                                           stores[i]->appliedStore(),
                                                           *feeds[i],
                                                           stores[i].get(),
                                                           pools[i].get(),
                                                           1500ms,
                                                           route_poll_period_);
            grpc::ServerBuilder builder;
            chronolog::rpc::applyServerPolicy(builder);
            builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
            builder.AddListeningPort(peers[i].internal_endpoint, grpc::InsecureServerCredentials());
            builder.RegisterService(services[i].get());
            servers[i] = builder.BuildAndStart();
            if(!servers[i])
                return absl::UnavailableError("cluster RPC port " + peers[i].internal_endpoint + " in use");
            channels[i] = grpc::CreateChannel(peers[i].internal_endpoint, grpc::InsecureChannelCredentials());
            stubs[i] = wire::Cluster::NewStub(channels[i]);
        }
        return absl::OkStatus();
    }
    void SetUp() override
    {
        absl::Status status;
        for(int attempt = 0; attempt < 8; ++attempt)
        {
            status = start(attempt);
            if(status.ok())
                break;
            for(size_t i = 0; i < 3; ++i)
            {
                stop(i);
                stubs[i].reset();
                channels[i].reset();
                feeds[i].reset();
                memberships[i].reset();
            }
            if(status.message().find(" in use") == std::string_view::npos)
                break;
        }
        ASSERT_TRUE(status.ok()) << status;
        size_t selected = leader();
        ASSERT_LT(selected, 3u);
        if(!seed_story_)
            return;
        ASSERT_TRUE(stores[selected]->createChronicle("c").ok());
        ASSERT_TRUE(stores[selected]->createStory("c", "s").ok());
    }
    bool seed_story_ = true;
    std::chrono::milliseconds route_poll_period_{100};
    size_t leader()
    {
        const auto until = std::chrono::steady_clock::now() + 8s;
        while(std::chrono::steady_clock::now() < until)
        {
            for(size_t i = 0; i < 3; ++i)
                if(stores[i] && stores[i]->leaderLease())
                    return i;
            std::this_thread::sleep_for(20ms);
        }
        return 3;
    }
    void stop(size_t i)
    {
        if(services[i])
            services[i]->shutdown();
        if(servers[i])
            servers[i]->Shutdown(std::chrono::system_clock::now() + 2s);
        servers[i].reset();
        pools[i].reset();
        services[i].reset();
        stores[i].reset();
    }
    void TearDown() override
    {
        for(size_t i = 0; i < 3; ++i) stop(i);
    }
};
TEST_F(DynamicClusterTest, FollowerForwardsKeeperDriverAndRouteFencesSurviveLeaderLoss)
{
    auto old = leader();
    ASSERT_LT(old, 3u);
    auto follower = (old + 1) % 3;
    KeeperDriver a{*stubs[follower], "keeper-a", "a1"};
    auto clock = std::make_shared<FakeClock>(100, 0);
    clock->setStatus(ClockStatus::Synced);
    auto membership = std::make_shared<keeper::ConfigMembership>();
    RamJournalConfig config;
    config.process_id = "keeper-b";
    config.instance = "b1";
    RamJournal journal(clock, membership, config);
    keeper::AcquisitionWatcher acquisitions(journal, "keeper-b", nullptr, false);
    keeper::ClusterClient b(channels[follower],
                            {"keeper-b", "b1", "keeper-b:50052"},
                            journal,
                            *membership,
                            acquisitions);
    ASSERT_EQ(a.Register().status().code(), 0);
    ASSERT_EQ(b.registerNow().code(), absl::StatusCode::kOk);
    ASSERT_EQ(a.Heartbeat().status().code(), 0);
    ASSERT_EQ(b.heartbeatNow().code(), absl::StatusCode::kOk);
    ASSERT_EQ(a.ExtendCeiling().status().code(), 0);
    ASSERT_EQ(b.extendNow().code(), absl::StatusCode::kOk);
    std::string remapped_identity;
    for(int attempt = 0; attempt < 32; ++attempt)
    {
        auto identity = "remapped-" + std::to_string(attempt);
        auto acquired = stores[old]->acquire(1, identity);
        ASSERT_TRUE(acquired.ok());
        if(acquired->assigned_keeper.process_id == "keeper-a")
        {
            remapped_identity = identity;
            break;
        }
    }
    ASSERT_FALSE(remapped_identity.empty());
    wire::DrainKeeperRequest request;
    request.set_process_id("keeper-a");
    wire::DrainKeeperResponse response;
    ASSERT_TRUE(stubs[follower]->DrainKeeper(a.context().get(), request, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
    ASSERT_EQ(response.routes_size(), 1);
    auto revision = response.routes(0).revision();
    ASSERT_GT(revision, 0u);
    EXPECT_EQ(response.routes(0).route().epoch(), 2u);
    ASSERT_TRUE(b.extendNow().ok());
    EXPECT_EQ(journal.appliedRouteRevision(), revision);
    EXPECT_EQ(membership->route(1)->epoch, 2);
    auto acquired = stores[old]->acquire(1, remapped_identity);
    ASSERT_TRUE(acquired.ok());
    EXPECT_EQ(acquired->assigned_keeper.process_id, "keeper-b");
    ASSERT_TRUE(journal.registerWriter(1, acquired->writer_id, acquired->incarnation).ok());
    AppendItem item;
    item.writer_id = acquired->writer_id;
    item.incarnation = acquired->incarnation;
    item.sequence = 1;
    item.physical = {100, 0, ClockStatus::Synced};
    auto appended = journal.append({1, 2, {item}}, Durability::Accepted);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok()) << appended->front().status;
    EXPECT_GT(appended->front().hlc,
              (Hlc{response.routes(0).ordering_cut().physical_ns(), response.routes(0).ordering_cut().logical()}));
    stop(old);
    auto next = leader();
    ASSERT_LT(next, 3u);
    keeper::ClusterClient resumed(channels[next],
                                  {"keeper-b", "b1", "keeper-b:50052"},
                                  journal,
                                  *membership,
                                  acquisitions);
    ASSERT_TRUE(resumed.heartbeatNow().ok());
    ASSERT_TRUE(resumed.extendNow().ok());
    EXPECT_EQ(journal.appliedRouteRevision(), revision);
    auto state = dynamic::snapshot(stores[next]->appliedStore());
    ASSERT_EQ(state.routes_size(), 1);
    EXPECT_EQ(state.routes(0).predecessors(0).instance(), "a1");
    EXPECT_EQ(state.routes(0).revision(), revision);
}
TEST_F(DynamicClusterTest, ForwardingToTheLeaderReusesOneChannelPerPeer)
{
    auto old = leader();
    ASSERT_LT(old, 3u);
    KeeperDriver a{*stubs[(old + 1) % 3], "keeper-a", "a1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    const auto after_first = rpc::ChannelPool::peers().created();
    EXPECT_GE(after_first, 1u);
    for(int call = 0; call < 20; ++call) ASSERT_EQ(a.Heartbeat().status().code(), 0);
    EXPECT_EQ(rpc::ChannelPool::peers().created(), after_first);
    EXPECT_LE(after_first, 3u);
}

TEST_F(DynamicClusterTest, WatchAndRefusedExtensionsKeepIntermediateObserveFloorUpdates)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    KeeperDriver a{*stubs[selected], "keeper-a", "a1"}, b{*stubs[selected], "keeper-b", "b1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    ASSERT_EQ(b.Register().status().code(), 0);
    ASSERT_EQ(a.ExtendCeiling().status().code(), 0);
    ASSERT_EQ(b.ExtendCeiling().status().code(), 0);
    auto first_writer = stores[selected]->acquire(1, "first");
    auto second_writer = stores[selected]->acquire(1, "second");
    ASSERT_TRUE(first_writer.ok());
    ASSERT_TRUE(second_writer.ok());
    ASSERT_EQ(second_writer->assigned_keeper.process_id, "keeper-a");
    auto context = a.context();
    wire::WatchRoutesRequest watch;
    auto reader = stubs[selected]->WatchRoutes(context.get(), watch);
    wire::WatchRoutesResponse first;
    ASSERT_TRUE(reader->Read(&first));
    EXPECT_EQ(first.route().epoch(), 1u);
    wire::DrainKeeperRequest drain;
    drain.set_process_id("keeper-a");
    wire::DrainKeeperResponse drained;
    ASSERT_TRUE(stubs[selected]->DrainKeeper(a.context().get(), drain, &drained).ok());
    ASSERT_EQ(drained.status().code(), 0);
    wire::JoinKeeperRequest join;
    join.set_process_id("keeper-a");
    wire::JoinKeeperResponse joined;
    ASSERT_TRUE(stubs[selected]->JoinKeeper(a.context().get(), join, &joined).ok());
    ASSERT_EQ(joined.status().code(), 0);
    auto refused = b.ExtendCeiling();
    ASSERT_NE(refused.status().code(), 0);
    ASSERT_EQ(refused.routes_size(), 2);
    EXPECT_EQ(refused.routes(0).route().epoch(), 2u);
    EXPECT_EQ(refused.routes(1).route().epoch(), 3u);
    auto observe = refused.routes(0).observe_floor();
    EXPECT_NE(std::find(observe.begin(), observe.end(), "keeper-b"), observe.end());
    wire::WatchRoutesResponse second, third;
    ASSERT_TRUE(reader->Read(&second));
    ASSERT_TRUE(reader->Read(&third));
    EXPECT_EQ(second.route().epoch(), 2u);
    EXPECT_EQ(third.route().epoch(), 3u);
    EXPECT_LT(second.revision(), third.revision());
    context->TryCancel();
    (void)reader->Finish();
}
TEST_F(DynamicClusterTest, AcquisitionWatchIsRefusedByAReplicaWithoutALeaderLease)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    stop((selected + 1) % 3);
    stop((selected + 2) % 3);
    const auto until = std::chrono::steady_clock::now() + 5s;
    while(stores[selected]->leaderLease() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(20ms);
    ASSERT_FALSE(stores[selected]->leaderLease());
    wire::WatchAcquisitionsRequest request;
    request.set_keeper_id("keeper-a");
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    auto reader = stubs[selected]->WatchAcquisitions(&context, request);
    wire::WatchAcquisitionsResponse message;
    EXPECT_FALSE(reader->Read(&message));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::UNAVAILABLE);
}

TEST_F(DynamicClusterTest, WatchRoutesReconnectOmitsStoryAndChronicleTombstones)
{
    const auto selected = leader();
    ASSERT_LT(selected, 3u);
    const auto kept = stores[selected]->createStory("c", "kept");
    ASSERT_TRUE(kept.ok()) << kept.status();
    ASSERT_TRUE(stores[selected]->createChronicle("removed").ok());
    ASSERT_TRUE(stores[selected]->createStory("removed", "first").ok());
    ASSERT_TRUE(stores[selected]->createStory("removed", "second").ok());
    grpc::ClientContext initial;
    rpc::withTimeout(initial, 10s);
    auto reader = stubs[selected]->WatchRoutes(&initial, wire::WatchRoutesRequest());
    wire::WatchRoutesResponse update;
    for(int i = 0; i < 4; ++i) ASSERT_TRUE(reader->Read(&update));
    initial.TryCancel();
    (void)reader->Finish();
    ASSERT_TRUE(stores[selected]->destroyStory(1).ok());
    ASSERT_TRUE(stores[selected]->destroyChronicle("removed").ok());

    grpc::ClientContext resumed;
    rpc::withTimeout(resumed, 10s);
    auto reconnect = stubs[selected]->WatchRoutes(&resumed, wire::WatchRoutesRequest());
    ASSERT_TRUE(reconnect->Read(&update));
    EXPECT_EQ(update.story_id(), kept->id);
    EXPECT_FALSE(update.tombstoned());
    EXPECT_EQ(update.revision(), stores[selected]->appliedStore().membershipRevision().value_or(0));
    resumed.TryCancel();
    (void)reconnect->Finish();
}
TEST_F(DynamicClusterTest, PlainHeartbeatsDoNotAppendRaftEntries)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    KeeperDriver a{*stubs[(selected + 1) % 3], "keeper-a", "a1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    auto before = stores[selected]->appliedStore().appliedIndex().value_or(0);
    for(int n = 0; n < 20; ++n) ASSERT_EQ(a.Heartbeat().status().code(), 0);
    EXPECT_EQ(stores[selected]->appliedStore().appliedIndex().value_or(0), before);
}
TEST_F(DynamicClusterTest, FailureDetectionKeepsLastKeeperAndRetriesLater)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    KeeperDriver a{*stubs[selected], "keeper-a", "a1"}, b{*stubs[selected], "keeper-b", "b1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    ASSERT_EQ(b.Register().status().code(), 0);
    ASSERT_EQ(a.ExtendCeiling().status().code(), 0);
    ASSERT_EQ(b.ExtendCeiling().status().code(), 0);
    wire::DrainKeeperRequest q;
    q.set_process_id("keeper-a");
    wire::DrainKeeperResponse r;
    ASSERT_TRUE(stubs[selected]->DrainKeeper(a.context().get(), q, &r).ok());
    ASSERT_EQ(r.status().code(), 0);
    for(int n = 0; n < 40; ++n) std::this_thread::sleep_for(50ms);
    auto state = dynamic::snapshot(stores[selected]->appliedStore());
    ASSERT_EQ(state.routes(0).route().keepers_size(), 1);
    EXPECT_EQ(state.routes(0).route().keepers(0).process_id(), "keeper-b");
    wire::JoinKeeperRequest join;
    join.set_process_id("keeper-a");
    wire::JoinKeeperResponse joined;
    ASSERT_TRUE(stubs[selected]->JoinKeeper(a.context().get(), join, &joined).ok());
    ASSERT_EQ(joined.status().code(), 0);
    bool retried = false;
    for(int n = 0; n < 200; ++n)
    {
        ASSERT_EQ(a.Heartbeat().status().code(), 0);
        state = dynamic::snapshot(stores[selected]->appliedStore());
        if(state.routes(0).route().keepers_size() == 1 && state.routes(0).route().keepers(0).process_id() == "keeper-a")
        {
            retried = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(retried) << state.ShortDebugString();
}
TEST_F(DynamicClusterTest, FailureDetectionRemovesSilentKeeperAfterFullTimeout)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    KeeperDriver a{*stubs[selected], "keeper-a", "a1"}, b{*stubs[selected], "keeper-b", "b1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    ASSERT_EQ(b.Register().status().code(), 0);
    ASSERT_EQ(a.ExtendCeiling().status().code(), 0);
    ASSERT_EQ(b.ExtendCeiling().status().code(), 0);
    auto start = std::chrono::steady_clock::now();
    bool removed = false;
    for(int attempt = 0; attempt < 60; ++attempt)
    {
        ASSERT_EQ(a.Heartbeat().status().code(), 0);
        auto state = dynamic::snapshot(stores[selected]->appliedStore());
        if(state.routes(0).route().epoch() > 1)
        {
            removed = true;
            EXPECT_GE(std::chrono::steady_clock::now() - start, 1300ms);
            EXPECT_EQ(state.routes(0).route().keepers_size(), 1);
            EXPECT_EQ(state.routes(0).route().keepers(0).process_id(), "keeper-a");
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(removed);
}
class DynamicClusterBeforeFirstStoryTest: public DynamicClusterTest
{
public:
    DynamicClusterBeforeFirstStoryTest() { seed_story_ = false; }
};

TEST_F(DynamicClusterBeforeFirstStoryTest, WatchRoutesDeliversPolicyDowngradeAtSameEpoch)
{
    const auto selected = leader();
    ASSERT_LT(selected, 3u);
    for(const auto& id: {"keeper-a", "keeper-b", "grapher"})
    {
        wire::RegisterRequest request;
        request.set_policy_version(1);
        auto* process = request.mutable_process();
        process->set_process_id(id);
        process->set_instance("instance");
        process->set_endpoint(std::string(id) + ":50052");
        process->set_role(std::string(id) == "grapher" ? wire::PROCESS_ROLE_GRAPHER : wire::PROCESS_ROLE_KEEPER);
        wire::RegisterResponse response;
        grpc::ClientContext context;
        rpc::withTimeout(context, 4s);
        ASSERT_TRUE(stubs[selected]->Register(&context, request, &response).ok());
        ASSERT_EQ(response.status().code(), 0);
    }
    ASSERT_TRUE(stores[selected]->createChronicle("c").ok());
    auto story = stores[selected]->createStory("c", "s");
    auto barrier = stores[selected]->createStory("c", "barrier");
    ASSERT_TRUE(story.ok());
    ASSERT_TRUE(barrier.ok());
    grpc::ClientContext watching;
    rpc::withTimeout(watching, 10s);
    auto reader = stubs[selected]->WatchRoutes(&watching, wire::WatchRoutesRequest());
    wire::WatchRoutesResponse update;
    uint64_t snapshot_revision = 0;
    for(int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(reader->Read(&update));
        EXPECT_TRUE(update.physical_policy());
        snapshot_revision = std::max(snapshot_revision, update.revision());
    }
    wire::HeartbeatRequest heartbeat;
    heartbeat.set_process_id("grapher");
    heartbeat.set_instance("instance");
    heartbeat.add_stories_without_physical_policy(story->id);
    wire::HeartbeatResponse response;
    grpc::ClientContext context;
    rpc::withTimeout(context, 4s);
    ASSERT_TRUE(stubs[selected]->Heartbeat(&context, heartbeat, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
    ASSERT_TRUE(stores[selected]->destroyStory(barrier->id).ok());
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), story->id);
    EXPECT_EQ(update.route().epoch(), 1u);
    EXPECT_FALSE(update.physical_policy());
    EXPECT_FALSE(update.tombstoned());
    EXPECT_GT(update.revision(), snapshot_revision);
    const auto downgraded_revision = update.revision();
    ASSERT_TRUE(reader->Read(&update));
    EXPECT_EQ(update.story_id(), barrier->id);
    EXPECT_TRUE(update.tombstoned());
    EXPECT_GT(update.revision(), downgraded_revision);
    watching.TryCancel();
    (void)reader->Finish();
}

// I4.3 and I4.9: a configured Keeper that has not registered yet is not failed, whatever the timeout.
TEST_F(DynamicClusterBeforeFirstStoryTest, FailureDetectionIgnoresConfiguredKeeperThatHasNotRegistered)
{
    auto selected = leader();
    ASSERT_LT(selected, 3u);
    KeeperDriver a{*stubs[selected], "keeper-a", "a1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    const auto until = std::chrono::steady_clock::now() + 3500ms;
    while(std::chrono::steady_clock::now() < until)
    {
        ASSERT_EQ(a.Heartbeat().status().code(), 0);
        std::this_thread::sleep_for(50ms);
    }
    auto state = stores[selected]->appliedStore().membershipLivenessState();
    ASSERT_TRUE(state.ok());
    bool seen = false;
    for(const auto& member: state->members())
        if(member.process().process_id() == "keeper-b")
        {
            seen = true;
            EXPECT_TRUE(member.joined());
        }
    EXPECT_TRUE(seen);
    ASSERT_TRUE(stores[selected]->createChronicle("c").ok());
    auto story = stores[selected]->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    auto route = stores[selected]->appliedStore().membershipRouteUpdate(story->id);
    ASSERT_TRUE(route.ok());
    EXPECT_EQ(route->route().keepers_size(), 2);
}
} // namespace
} // namespace chronolog::visor

namespace chronolog::visor
{
class DynamicRouteWakeTest: public DynamicClusterTest
{
    void SetUp() override
    {
        seed_story_ = false;
        route_poll_period_ = 1h;
        DynamicClusterTest::SetUp();
    }
};
TEST_F(DynamicRouteWakeTest, EveryReplicaReceivesRoutesWithoutPeriodicTick)
{
    const auto selected = leader();
    ASSERT_LT(selected, 3u);
    for(const auto& id: {"keeper-a", "keeper-b", "grapher"})
    {
        wire::CatalogCommand command;
        auto* request = command.mutable_membership()->mutable_register_();
        request->set_policy_version(1);
        auto* process = request->mutable_process();
        process->set_process_id(id);
        process->set_instance("instance");
        process->set_endpoint(std::string(id) + ":50052");
        process->set_role(std::string(id) == "grapher" ? wire::PROCESS_ROLE_GRAPHER : wire::PROCESS_ROLE_KEEPER);
        auto result = stores[selected]->propose(command);
        ASSERT_TRUE(result.ok()) << result.status();
        wire::RegisterResponse response;
        ASSERT_TRUE(response.ParseFromString(*result));
        ASSERT_EQ(response.status().code(), 0);
    }
    ASSERT_TRUE(stores[selected]->createChronicle("wake").ok());
    auto barrier = stores[selected]->createStory("wake", "barrier");
    ASSERT_TRUE(barrier.ok());
    std::array<grpc::ClientContext, 3> contexts;
    std::array<std::unique_ptr<grpc::ClientReader<wire::WatchRoutesResponse>>, 3> readers;
    std::array<uint64_t, 3> revisions{};
    for(size_t i = 0; i < 3; ++i)
    {
        contexts[i].set_deadline(std::chrono::system_clock::now() + 15s);
        readers[i] = stubs[i]->WatchRoutes(&contexts[i], wire::WatchRoutesRequest());
        wire::WatchRoutesResponse update;
        ASSERT_TRUE(readers[i]->Read(&update));
        ASSERT_EQ(update.story_id(), barrier->id);
        revisions[i] = update.revision();
    }
    auto created = stores[selected]->createStory("wake", "created");
    ASSERT_TRUE(created.ok());
    for(size_t i = 0; i < 3; ++i)
    {
        wire::WatchRoutesResponse update;
        ASSERT_TRUE(readers[i]->Read(&update));
        EXPECT_EQ(update.story_id(), created->id);
        EXPECT_TRUE(update.physical_policy());
        EXPECT_GT(update.revision(), revisions[i]);
        revisions[i] = update.revision();
    }
    wire::CatalogCommand downgrade;
    auto* heartbeat = downgrade.mutable_membership()->mutable_heartbeat();
    heartbeat->set_process_id("grapher");
    heartbeat->set_instance("instance");
    heartbeat->add_stories_without_physical_policy(created->id);
    auto result = stores[selected]->propose(downgrade);
    ASSERT_TRUE(result.ok());
    wire::HeartbeatResponse response;
    ASSERT_TRUE(response.ParseFromString(*result));
    ASSERT_EQ(response.status().code(), 0);
    for(size_t i = 0; i < 3; ++i)
    {
        wire::WatchRoutesResponse update;
        ASSERT_TRUE(readers[i]->Read(&update));
        EXPECT_EQ(update.story_id(), created->id);
        EXPECT_FALSE(update.physical_policy());
        EXPECT_GT(update.revision(), revisions[i]);
        revisions[i] = update.revision();
    }
    ASSERT_TRUE(stores[selected]->destroyStory(created->id).ok());
    for(size_t i = 0; i < 3; ++i)
    {
        wire::WatchRoutesResponse update;
        ASSERT_TRUE(readers[i]->Read(&update));
        EXPECT_EQ(update.story_id(), created->id);
        EXPECT_TRUE(update.tombstoned());
        EXPECT_GT(update.revision(), revisions[i]);
        contexts[i].TryCancel();
        (void)readers[i]->Finish();
    }
}
} // namespace chronolog::visor
