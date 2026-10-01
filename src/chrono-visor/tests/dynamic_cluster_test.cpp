#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "adapter/WorkerPool.h"
#include "dynamic/MembershipState.h"
#include "raft/RaftMetadataStore.h"
#include <gtest/gtest.h>
#include <array>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
namespace chronolog::visor
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;
int freePort()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
    {
        close(fd);
        throw std::runtime_error("bind failed");
    }
    socklen_t length = sizeof(address);
    if(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length))
    {
        close(fd);
        throw std::runtime_error("getsockname failed");
    }
    int port = ntohs(address.sin_port);
    close(fd);
    return port;
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
    void SetUp() override
    {
        std::vector<RaftPeer> peers;
        for(int i = 0; i < 3; ++i)
        {
            auto raft_endpoint = "127.0.0.1:" + std::to_string(freePort());
            auto endpoint = "127.0.0.1:" + std::to_string(freePort());
            peers.push_back({i + 1, raft_endpoint, endpoint, endpoint});
        }
        for(size_t i = 0; i < 3; ++i)
        {
            auto opened = RaftMetadataStore::open((dir.path() / std::to_string(i)).string(),
                                                  testing::twoKeeperTopology(),
                                                  {static_cast<int32_t>(i + 1), peers[i].raft_endpoint, peers});
            ASSERT_TRUE(opened.ok()) << opened.status();
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
                                                           1500ms);
            grpc::ServerBuilder builder;
            builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
            builder.AddListeningPort(peers[i].internal_endpoint, grpc::InsecureServerCredentials());
            builder.RegisterService(services[i].get());
            servers[i] = builder.BuildAndStart();
            ASSERT_NE(servers[i], nullptr);
            stubs[i] = wire::Cluster::NewStub(
                    grpc::CreateChannel(peers[i].internal_endpoint, grpc::InsecureChannelCredentials()));
        }
        size_t selected = leader();
        ASSERT_LT(selected, 3u);
        ASSERT_TRUE(stores[selected]->createChronicle("c").ok());
        ASSERT_TRUE(stores[selected]->createStory("c", "s").ok());
    }
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
    KeeperDriver a{*stubs[follower], "keeper-a", "a1"}, b{*stubs[follower], "keeper-b", "b1"};
    ASSERT_EQ(a.Register().status().code(), 0);
    ASSERT_EQ(b.Register().status().code(), 0);
    ASSERT_EQ(a.Heartbeat().status().code(), 0);
    ASSERT_EQ(b.Heartbeat().status().code(), 0);
    ASSERT_EQ(a.ExtendCeiling().status().code(), 0);
    ASSERT_EQ(b.ExtendCeiling().status().code(), 0);
    wire::KeeperRequest request;
    request.set_process_id("keeper-a");
    wire::MembershipResponse response;
    ASSERT_TRUE(stubs[follower]->DrainKeeper(a.context().get(), request, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
    ASSERT_EQ(response.routes_size(), 1);
    auto revision = response.routes(0).revision();
    ASSERT_GT(revision, 0u);
    EXPECT_EQ(response.routes(0).route().epoch(), 2u);
    auto refused = b.ExtendCeiling();
    ASSERT_NE(refused.status().code(), 0);
    ASSERT_EQ(refused.routes_size(), 1);
    EXPECT_EQ(refused.fence_revision(), revision);
    ASSERT_EQ(b.ExtendCeiling().status().code(), 0);
    stop(old);
    auto next = leader();
    ASSERT_LT(next, 3u);
    KeeperDriver resumed{*stubs[next], "keeper-b", "b1", revision};
    ASSERT_EQ(resumed.Heartbeat().status().code(), 0);
    auto renewed = resumed.ExtendCeiling();
    ASSERT_EQ(renewed.status().code(), 0);
    EXPECT_EQ(renewed.fence_revision(), revision);
    auto state = dynamic::snapshot(stores[next]->appliedStore());
    ASSERT_EQ(state.routes_size(), 1);
    EXPECT_EQ(state.routes(0).predecessors(0).instance(), "a1");
    EXPECT_EQ(state.routes(0).revision(), revision);
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
    wire::KeeperRequest request;
    request.set_process_id("keeper-a");
    wire::MembershipResponse response;
    ASSERT_TRUE(stubs[selected]->DrainKeeper(a.context().get(), request, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
    ASSERT_TRUE(stubs[selected]->JoinKeeper(a.context().get(), request, &response).ok());
    ASSERT_EQ(response.status().code(), 0);
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
    wire::KeeperRequest q;
    q.set_process_id("keeper-a");
    wire::MembershipResponse r;
    ASSERT_TRUE(stubs[selected]->DrainKeeper(a.context().get(), q, &r).ok());
    ASSERT_EQ(r.status().code(), 0);
    for(int n = 0; n < 40; ++n) std::this_thread::sleep_for(50ms);
    auto state = dynamic::snapshot(stores[selected]->appliedStore());
    ASSERT_EQ(state.routes(0).route().keepers_size(), 1);
    EXPECT_EQ(state.routes(0).route().keepers(0).process_id(), "keeper-b");
    ASSERT_TRUE(stubs[selected]->JoinKeeper(a.context().get(), q, &r).ok());
    ASSERT_EQ(r.status().code(), 0);
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
    EXPECT_TRUE(retried);
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
} // namespace
} // namespace chronolog::visor
