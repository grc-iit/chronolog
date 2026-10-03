#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <mutex>
#include <string>
#include <vector>

#include "membership/AcquisitionWatcher.h"
#include "membership/ConfigMembership.h"
#include "ram_harness.h"
#include "runtime/ClusterClient.h"

namespace chronolog
{
namespace
{
namespace iv1 = chronolog::internal::v1;

class CapturingCluster final: public iv1::Cluster::Service
{
public:
    grpc::Status Heartbeat(grpc::ServerContext*, const iv1::HeartbeatRequest* request, iv1::HeartbeatResponse*) override
    {
        std::lock_guard lock(mutex);
        last = *request;
        return fail ? grpc::Status(grpc::StatusCode::UNAVAILABLE, "lost delivery") : grpc::Status::OK;
    }
    bool fail{false};
    std::mutex mutex;
    iv1::HeartbeatRequest last;
};

TEST(ClusterClientTest, FailedHeartbeatDoesNotReplayEvidenceForever)
{
    CapturingCluster cluster;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cluster);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    test::RamRig rig;
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient client(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()),
            {"self", "instance", "self:1", std::chrono::milliseconds(5000)},
            *rig.journal,
            membership,
            watcher);
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    ASSERT_TRUE(rig.journal->append({1, 7, {item}}, Durability::Accepted).ok());
    {
        std::lock_guard lock(cluster.mutex);
        cluster.fail = true;
    }
    EXPECT_FALSE(client.heartbeatNow().ok());
    {
        std::lock_guard lock(cluster.mutex);
        ASSERT_EQ(cluster.last.admission_evidence_size(), 1);
        EXPECT_EQ(cluster.last.process_id(), "self");
        EXPECT_EQ(cluster.last.instance(), "instance");
        EXPECT_EQ(cluster.last.admission_evidence(0).story_id(), 1u);
        EXPECT_EQ(cluster.last.admission_evidence(0).writer_id(), 2u);
        EXPECT_EQ(cluster.last.admission_evidence(0).incarnation(), 3u);
        cluster.fail = false;
    }
    ASSERT_TRUE(client.heartbeatNow().ok());
    {
        std::lock_guard lock(cluster.mutex);
        EXPECT_EQ(cluster.last.admission_evidence_size(), 0);
    }
    item.sequence = 2;
    ASSERT_TRUE(rig.journal->append({1, 7, {item}}, Durability::Accepted).ok());
    ASSERT_TRUE(client.heartbeatNow().ok());
    {
        std::lock_guard lock(cluster.mutex);
        EXPECT_EQ(cluster.last.admission_evidence_size(), 1);
    }
    server->Shutdown(std::chrono::system_clock::now());
}

// I4.14: the leader removes a drained predecessor only when the report carries a sealed frontier at
// or above the cut (MembershipState heartbeat apply), so the Keeper must send it with the report.
TEST(ClusterClientTest, DrainReportCarriesTheSealedFrontierAtItsOwnCut)
{
    CapturingCluster cluster;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cluster);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    test::RamRig rig;
    const Hlc cut{2'000'000'000, 0};
    rig.clock->setPhysical(3'000'000'000);
    rig.journal->enableDynamic("instance");
    rig.journal->extendCeiling({10'000'000'000, 0}, 20'000'000'000);
    RouteState state;
    state.route = {8, {{"other", "other:1"}}, "127.0.0.1:1", ""};
    state.predecessors.push_back({{"self", "self:1"}, "instance", 7, cut, 20'000'000'000});
    rig.journal->applyRoute(1, state, false, 10, [] {});
    rig.journal->eraseEvents(1, {Range::Axis::Hlc, {}, cut}, true);

    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient client(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()),
            {"self", "instance", "self:1", std::chrono::milliseconds(5000)},
            *rig.journal,
            membership,
            watcher);
    ASSERT_TRUE(client.heartbeatNow().ok());
    std::lock_guard lock(cluster.mutex);
    int drains = 0;
    for(const auto& entry: cluster.last.story_frontiers())
        if(!entry.drained_instance().empty())
        {
            ++drains;
            EXPECT_EQ(entry.drained_instance(), "instance");
            EXPECT_EQ(entry.drained_epoch(), 7u);
            EXPECT_GE(entry.sealed_frontier().physical_ns(), cut.physical_ns);
            EXPECT_GE(entry.evicted_below().physical_ns(), cut.physical_ns);
        }
    EXPECT_EQ(drains, 1);
    server->Shutdown(std::chrono::system_clock::now());
}

class ReplicatedCluster final: public iv1::Cluster::Service
{
public:
    grpc::Status Register(grpc::ServerContext*, const iv1::RegisterRequest*, iv1::RegisterResponse* response) override
    {
        for(int n = 0; n < 3; ++n) response->add_visor_replicas(self);
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext* context, const iv1::HeartbeatRequest*, iv1::HeartbeatResponse*) override
    {
        std::lock_guard lock(mutex);
        budgets.push_back(context->deadline() - std::chrono::system_clock::now());
        return budgets.size() == 1 ? grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease") : grpc::Status::OK;
    }
    std::string self;
    std::mutex mutex;
    std::vector<std::chrono::system_clock::duration> budgets;
};

// The attached follower forwards to the leader, which may answer late under load: with three replicas known, the
// attempt that does the work still gets the call's whole remaining deadline instead of a quarter of it, and an
// UNAVAILABLE answer still moves on to the next replica within the same deadline.
TEST(ClusterClientTest, AnAttemptGetsTheWholeRemainingDeadline)
{
    ReplicatedCluster cluster;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&cluster);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    cluster.self = "127.0.0.1:" + std::to_string(port);
    test::RamRig rig;
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    const auto deadline = std::chrono::milliseconds(4000);
    keeper::ClusterClient client(grpc::CreateChannel(cluster.self, grpc::InsecureChannelCredentials()),
                                 {"self", "instance", "self:1", std::chrono::milliseconds(5000), "", deadline},
                                 *rig.journal,
                                 membership,
                                 watcher);
    ASSERT_TRUE(client.registerNow().ok());
    ASSERT_TRUE(client.heartbeatNow().ok());
    server->Shutdown(std::chrono::system_clock::now());
    std::lock_guard lock(cluster.mutex);
    ASSERT_EQ(cluster.budgets.size(), 2u);
    EXPECT_GT(cluster.budgets[0], deadline / 2);
    EXPECT_GT(cluster.budgets[1], deadline / 2);
}
} // namespace
} // namespace chronolog
