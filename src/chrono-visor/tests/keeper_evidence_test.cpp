// Keeper admission evidence (HeartbeatRequest field 8) through a real Cluster channel in
// dynamic (one-replica Raft) and static (SQLite) membership: only the current registered
// instance renews its current assigned unreleased tuple, leader-locally, with no proposal.
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <random>
#include <thread>

#include "TestSupport.h"
#include "adapter/ClusterService.h"
#include "adapter/WorkerPool.h"
#include "raft/RaftMetadataStore.h"

namespace chronolog::visor
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;

int loopbackPort()
{
    static std::mt19937 random(std::random_device{}());
    for(int attempt = 0; attempt < 64; ++attempt)
    {
        const int port = 10000 + (random() % 4400) * 5;
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
            return port;
    }
    throw std::runtime_error("no free loopback port");
}

class EvidenceRig: public ::testing::Test
{
protected:
    explicit EvidenceRig(bool dynamic)
        : dynamic_(dynamic)
    {}
    void SetUp() override
    {
        if(dynamic_)
        {
            absl::Status status;
            for(int attempt = 0; attempt < 8 && !raft_; ++attempt)
            {
                const auto endpoint = "127.0.0.1:" + std::to_string(loopbackPort());
                auto opened = RaftMetadataStore::open((dir_.path() / std::to_string(attempt)).string(),
                                                      testing::twoKeeperTopology(),
                                                      {1, endpoint, {{1, endpoint, endpoint, endpoint}}},
                                                      nullptr,
                                                      {},
                                                      control_);
                status = opened.status();
                if(opened.ok())
                    raft_ = std::move(*opened);
            }
            ASSERT_TRUE(raft_) << status;
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            while(!raft_->leaderLease() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            ASSERT_TRUE(raft_->leaderLease());
            store_ = raft_.get();
            applied_ = &raft_->appliedStore();
            leases_ = &raft_->leaseAuthority();
        }
        else
        {
            auto opened = SqliteMetadataStore::open((dir_.path() / "catalog").string(), testing::twoKeeperTopology());
            ASSERT_TRUE(opened.ok()) << opened.status();
            sqlite_ = std::move(*opened);
            store_ = sqlite_.get();
            applied_ = sqlite_.get();
            leases_ = &sqlite_->leaseAuthority();
        }
        membership_ = std::make_unique<StaticRouteMembership>(
                testing::twoKeeperTopology(),
                1,
                [this](StoryId id) { return applied_->getStory(id).ok(); },
                15s);
        feed_ = std::make_unique<AcquisitionFeed>();
        pool_ = std::make_unique<WorkerPool>(2, 64);
        service_ = std::make_unique<ClusterService>(*membership_,
                                                    *applied_,
                                                    *applied_,
                                                    *feed_,
                                                    raft_.get(),
                                                    pool_.get(),
                                                    15s);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = wire::Cluster::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
        ASSERT_TRUE(store_->createChronicle("c").ok());
        auto story = store_->createStory("c", "s");
        ASSERT_TRUE(story.ok());
        story_ = story->id;
    }
    void TearDown() override
    {
        if(service_)
            service_->shutdown();
        if(server_)
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
        pool_.reset();
    }
    std::unique_ptr<grpc::ClientContext> context()
    {
        auto c = std::make_unique<grpc::ClientContext>();
        c->set_deadline(std::chrono::system_clock::now() + 10s);
        return c;
    }
    int registerKeeper(const std::string& id, const std::string& instance)
    {
        wire::RegisterRequest q;
        auto* p = q.mutable_process();
        p->set_process_id(id);
        p->set_instance(instance);
        p->set_endpoint(id + ":50052");
        p->set_role(wire::PROCESS_ROLE_KEEPER);
        wire::RegisterResponse r;
        if(!stub_->Register(context().get(), q, &r).ok())
            return -1;
        for(const auto& route: r.routes()) route_revision_ = std::max(route_revision_, route.revision());
        return r.status().code();
    }
    // Dynamic Keeper replacement needs a granted ceiling for every old route Keeper.
    int extendCeiling(const std::string& id, const std::string& instance)
    {
        wire::ExtendCeilingRequest q;
        q.set_process_id(id);
        q.set_instance(instance);
        q.set_applied_route_revision(route_revision_);
        q.set_realtime_ns(100);
        q.mutable_wanted_hlc()->set_physical_ns(100);
        wire::ExtendCeilingResponse r;
        return stub_->ExtendCeiling(context().get(), q, &r).ok() ? r.status().code() : -1;
    }
    // Item status code of the heartbeat, or -1 when the RPC itself failed.
    int heartbeat(const std::string& id,
                  const std::string& instance,
                  const std::vector<RenewAcquisition>& evidence,
                  bool change = false)
    {
        wire::HeartbeatRequest q;
        q.set_process_id(id);
        q.set_instance(instance);
        for(const auto& t: evidence)
        {
            auto* e = q.add_admission_evidence();
            e->set_story_id(t.story_id);
            e->set_writer_id(t.writer_id);
            e->set_incarnation(t.incarnation);
        }
        if(change)
            q.add_stories_without_physical_policy(0);
        wire::HeartbeatResponse r;
        return stub_->Heartbeat(context().get(), q, &r).ok() ? r.status().code() : -1;
    }
    // Remaining local lease time, or -1 when the tuple is not live.
    int64_t remaining(const Acquisition& grant)
    {
        auto rows = applied_->acquisitionRows({{grant.story_id, grant.writer_id, grant.incarnation}});
        if(!rows.ok() || rows->front().state != AcquisitionState::Acquired)
            return -1;
        auto lease = leases_->sample(rows->front(), false);
        return lease.ok() ? lease->remaining_ns : -1;
    }
    void advance(int64_t ns) { leases_->advanceClock(ns, true); }
    Acquisition acquire(const std::string& identity)
    {
        auto grant = store_->acquire(story_, identity);
        if(!grant.ok())
            throw std::runtime_error(grant.status().ToString());
        return *grant;
    }
    static RenewAcquisition tuple(const Acquisition& g) { return {g.story_id, g.writer_id, g.incarnation}; }
    // Registers both Keepers and returns the instance of each.
    void registerBoth()
    {
        ASSERT_EQ(registerKeeper("keeper-a", "a1"), 0);
        ASSERT_EQ(registerKeeper("keeper-b", "b1"), 0);
    }
    static std::string instanceOf(const std::string& id) { return id == "keeper-a" ? "a1" : "b1"; }
    static std::string otherThan(const std::string& id) { return id == "keeper-a" ? "keeper-b" : "keeper-a"; }

    void keeperEvidenceRenewsOnlyCurrentAssignedTuple();
    void noAdmissionHeartbeatDoesNotRenewDeadHolder();
    void revokedInstanceCannotRenew();
    void heartbeatEvidenceAddsNoProposal();
    void failedHeartbeatDoesNotReplayEvidenceForever();

    bool dynamic_;
    testing::TempDir dir_;
    std::shared_ptr<RaftTestControl> control_ = std::make_shared<RaftTestControl>();
    std::unique_ptr<RaftMetadataStore> raft_;
    std::unique_ptr<SqliteMetadataStore> sqlite_;
    MetadataStore* store_{};
    SqliteMetadataStore* applied_{};
    LeaseAuthority* leases_{};
    std::unique_ptr<StaticRouteMembership> membership_;
    std::unique_ptr<AcquisitionFeed> feed_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<ClusterService> service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<wire::Cluster::Stub> stub_;
    StoryId story_{};
    uint64_t route_revision_{};
};
class ClusterAdapterTest: public EvidenceRig
{
protected:
    ClusterAdapterTest()
        : EvidenceRig(true)
    {}
};
class StaticClusterAdapterTest: public EvidenceRig
{
protected:
    StaticClusterAdapterTest()
        : EvidenceRig(false)
    {}
};

void EvidenceRig::keeperEvidenceRenewsOnlyCurrentAssignedTuple()
{
    registerBoth();
    const auto first = acquire("w");
    const auto owner = first.assigned_keeper.process_id;
    const auto T = first.lease.duration_ns;
    advance(T / 2);
    EXPECT_EQ(heartbeat(otherThan(owner), instanceOf(otherThan(owner)), {tuple(first)}), 0);
    EXPECT_LT(remaining(first), T * 5 / 8);
    EXPECT_EQ(heartbeat(owner,
                        instanceOf(owner),
                        {{story_, first.writer_id, first.incarnation + 5}, {story_, 99999, 1}, {0, 0, 0}}),
              0);
    EXPECT_LT(remaining(first), T * 5 / 8);
    EXPECT_EQ(heartbeat(owner, instanceOf(owner), {tuple(first)}), 0);
    EXPECT_GT(remaining(first), T * 7 / 8);

    // A superseded incarnation renews neither itself nor its successor.
    const auto second = acquire("w");
    ASSERT_GT(second.incarnation, first.incarnation);
    const auto holder = second.assigned_keeper.process_id;
    advance(T / 2);
    EXPECT_EQ(heartbeat(holder, instanceOf(holder), {tuple(first)}), 0);
    EXPECT_EQ(remaining(first), -1);
    EXPECT_LT(remaining(second), T * 5 / 8);
    EXPECT_EQ(heartbeat(holder, instanceOf(holder), {tuple(second)}), 0);
    EXPECT_GT(remaining(second), T * 7 / 8);

    // A released tuple is never reinstalled by evidence.
    ASSERT_TRUE(store_->release(story_, second.writer_id, second.incarnation).ok());
    EXPECT_EQ(leases_->size(), 0u);
    EXPECT_EQ(heartbeat(holder, instanceOf(holder), {tuple(first), tuple(second)}), 0);
    EXPECT_EQ(leases_->size(), 0u);
}
void EvidenceRig::noAdmissionHeartbeatDoesNotRenewDeadHolder()
{
    registerBoth();
    const auto grant = acquire("dead");
    const auto owner = grant.assigned_keeper.process_id;
    const auto T = grant.lease.duration_ns;
    for(int beat = 0; beat < 4; ++beat)
    {
        advance(T / 4);
        EXPECT_EQ(heartbeat(owner, instanceOf(owner), {}), 0);
    }
    advance(T / 8);
    EXPECT_EQ(remaining(grant), 0);
    // Evidence arriving after the deadline is due cannot revive the holder.
    EXPECT_EQ(heartbeat(owner, instanceOf(owner), {tuple(grant)}), 0);
    EXPECT_EQ(remaining(grant), 0);
}
void EvidenceRig::revokedInstanceCannotRenew()
{
    registerBoth();
    if(dynamic_)
    {
        ASSERT_EQ(extendCeiling("keeper-a", "a1"), 0);
        ASSERT_EQ(extendCeiling("keeper-b", "b1"), 0);
    }
    const auto grant = acquire("revoked");
    const auto owner = grant.assigned_keeper.process_id;
    const auto old = instanceOf(owner), fresh = old + "-restarted";
    const auto T = grant.lease.duration_ns;
    advance(T / 2);
    // Dynamic replacement also fences the old owner's writers; static registration only revokes the instance.
    ASSERT_EQ(registerKeeper(owner, fresh), 0);
    const int obsolete = static_cast<int>(absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(heartbeat(owner, old, {tuple(grant)}), obsolete);
    EXPECT_LT(remaining(grant), T * 5 / 8);
    const auto again = acquire("revoked");
    ASSERT_GT(again.incarnation, grant.incarnation);
    ASSERT_EQ(again.assigned_keeper.process_id, owner);
    advance(T / 2);
    EXPECT_EQ(heartbeat(owner, old, {tuple(again)}), obsolete);
    EXPECT_LT(remaining(again), T * 5 / 8);
    EXPECT_EQ(heartbeat(owner, fresh, {tuple(again)}), 0);
    EXPECT_GT(remaining(again), T * 7 / 8);
}
void EvidenceRig::heartbeatEvidenceAddsNoProposal()
{
    registerBoth();
    const auto grant = acquire("quiet");
    const auto owner = grant.assigned_keeper.process_id;
    const auto T = grant.lease.duration_ns;
    ASSERT_EQ(heartbeat(owner, instanceOf(owner), {}), 0);
    advance(T / 2);
    const auto proposals = control_->proposal_count.load();
    const auto index = applied_->appliedIndex().value_or(0);
    const auto changes = applied_->totalChanges();
    const auto revision = applied_->snapshotAcquisitions()->revision;
    for(int beat = 0; beat < 5; ++beat) ASSERT_EQ(heartbeat(owner, instanceOf(owner), {tuple(grant)}), 0);
    EXPECT_GT(remaining(grant), T * 7 / 8);
    EXPECT_EQ(control_->proposal_count.load(), proposals);
    EXPECT_EQ(applied_->appliedIndex().value_or(0), index);
    EXPECT_EQ(applied_->totalChanges(), changes);
    EXPECT_EQ(applied_->snapshotAcquisitions()->revision, revision);
    if(!dynamic_)
        return;
    // A heartbeat that must be proposed carries the evidence locally but not in the log entry.
    advance(T / 2);
    (void)heartbeat(owner, instanceOf(owner), {tuple(grant)}, true);
    EXPECT_GT(remaining(grant), T * 7 / 8);
    EXPECT_EQ(control_->proposal_count.load(), proposals + 1);
    wire::CatalogCommand command;
    ASSERT_TRUE(command.ParseFromString(control_->lastCommand()));
    ASSERT_TRUE(command.membership().has_heartbeat());
    EXPECT_EQ(command.membership().heartbeat().process_id(), owner);
    EXPECT_EQ(command.membership().heartbeat().admission_evidence_size(), 0);
}
void EvidenceRig::failedHeartbeatDoesNotReplayEvidenceForever()
{
    registerBoth();
    const auto grant = acquire("failed");
    const auto owner = grant.assigned_keeper.process_id;
    const auto T = grant.lease.duration_ns;
    advance(T / 2);
    if(dynamic_)
    {
        // An unqualified leader neither renews nor keeps the drained set for later.
        control_->qualification_enabled = false;
        EXPECT_EQ(heartbeat(owner, instanceOf(owner), {tuple(grant)}), -1);
        control_->qualification_enabled = true;
        ASSERT_TRUE(raft_->leaderLease());
    }
    else
        EXPECT_EQ(heartbeat(owner, "unknown-instance", {tuple(grant)}),
                  static_cast<int>(absl::StatusCode::kFailedPrecondition));
    EXPECT_LT(remaining(grant), T * 5 / 8);
    for(int beat = 0; beat < 3; ++beat)
    {
        EXPECT_EQ(heartbeat(owner, instanceOf(owner), {}), 0);
        EXPECT_LT(remaining(grant), T * 5 / 8);
    }
    advance(T * 5 / 8);
    EXPECT_EQ(heartbeat(owner, instanceOf(owner), {}), 0);
    EXPECT_EQ(remaining(grant), 0);
}

TEST_F(ClusterAdapterTest, KeeperEvidenceRenewsOnlyCurrentAssignedTuple)
{
    keeperEvidenceRenewsOnlyCurrentAssignedTuple();
}
TEST_F(StaticClusterAdapterTest, KeeperEvidenceRenewsOnlyCurrentAssignedTuple)
{
    keeperEvidenceRenewsOnlyCurrentAssignedTuple();
}
TEST_F(ClusterAdapterTest, NoAdmissionHeartbeatDoesNotRenewDeadHolder) { noAdmissionHeartbeatDoesNotRenewDeadHolder(); }
TEST_F(StaticClusterAdapterTest, NoAdmissionHeartbeatDoesNotRenewDeadHolder)
{
    noAdmissionHeartbeatDoesNotRenewDeadHolder();
}
TEST_F(ClusterAdapterTest, RevokedInstanceCannotRenew) { revokedInstanceCannotRenew(); }
TEST_F(StaticClusterAdapterTest, RevokedInstanceCannotRenew) { revokedInstanceCannotRenew(); }
TEST_F(ClusterAdapterTest, HeartbeatEvidenceAddsNoProposal) { heartbeatEvidenceAddsNoProposal(); }
TEST_F(StaticClusterAdapterTest, HeartbeatEvidenceAddsNoProposal) { heartbeatEvidenceAddsNoProposal(); }
TEST_F(ClusterAdapterTest, FailedHeartbeatDoesNotReplayEvidenceForever)
{
    failedHeartbeatDoesNotReplayEvidenceForever();
}
TEST_F(StaticClusterAdapterTest, FailedHeartbeatDoesNotReplayEvidenceForever)
{
    failedHeartbeatDoesNotReplayEvidenceForever();
}
} // namespace
} // namespace chronolog::visor
