#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>

#include <atomic>
#include <mutex>
#include <sstream>
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
using namespace std::chrono_literals;

constexpr int64_t kMs = 1'000'000;

// A Visor replica that stamps every reply with a physical reading `offset_ns` away from the Keeper clock's
// fixed reading and names `replica_id` as the generator.
class FakeVisor final: public iv1::Cluster::Service
{
public:
    grpc::Status Register(grpc::ServerContext*, const iv1::RegisterRequest*, iv1::RegisterResponse* response) override
    {
        std::lock_guard lock(mutex);
        if(fail)
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease");
        stamp(*response);
        for(const auto& replica: replicas) response->add_visor_replicas(replica);
        if(policy)
            *response->mutable_policy() = *policy;
        return grpc::Status::OK;
    }
    grpc::Status
    Heartbeat(grpc::ServerContext*, const iv1::HeartbeatRequest*, iv1::HeartbeatResponse* response) override
    {
        std::lock_guard lock(mutex);
        if(fail)
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease");
        stamp(*response);
        return grpc::Status::OK;
    }
    template <class Response>
    void stamp(Response& response)
    {
        response.mutable_physical()->set_physical_ns(base_ns + offset_ns);
        response.mutable_physical()->set_uncertainty_ns(kMs);
        response.mutable_physical()->set_status(v1::CLOCK_STATUS_SYNCED);
        response.mutable_clock_responder()->set_replica_id(replica_id);
        response.mutable_clock_responder()->set_instance(instance);
    }
    void set(int64_t offset)
    {
        std::lock_guard lock(mutex);
        offset_ns = offset;
    }

    std::mutex mutex;
    int64_t base_ns{5'000 * kMs};
    int64_t offset_ns{};
    bool fail{};
    std::string replica_id{"1"};
    std::string instance{"visor-a"};
    std::vector<std::string> replicas;
    std::optional<iv1::MembershipPolicy> policy;
};

struct Server
{
    explicit Server(FakeVisor& visor)
    {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&visor);
        server = builder.BuildAndStart();
    }
    ~Server() { server->Shutdown(std::chrono::system_clock::now()); }
    std::string endpoint() const { return "127.0.0.1:" + std::to_string(port); }
    int port{};
    std::unique_ptr<grpc::Server> server;
};

class CapturedLog final: public absl::LogSink
{
public:
    CapturedLog() { absl::AddLogSink(this); }
    ~CapturedLog() override { absl::RemoveLogSink(this); }
    void Send(const absl::LogEntry& entry) override
    {
        std::lock_guard lock(mutex_);
        lines_.emplace_back(entry.text_message());
    }
    size_t count(const std::string& needle)
    {
        std::lock_guard lock(mutex_);
        size_t n = 0;
        for(const auto& line: lines_) n += line.find(needle) != std::string::npos;
        return n;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

iv1::MembershipPolicy policyWith(uint32_t failure_ms, uint32_t fence_ms)
{
    const PhysicalPolicy expected;
    iv1::MembershipPolicy policy;
    policy.set_version(expected.version);
    policy.set_acceptance_window_ns(expected.acceptance_window_ns);
    policy.set_skew_limit_ns(expected.skew_limit_ns);
    policy.set_hlc_lead_ns(expected.hlc_lead_ns);
    policy.set_uncertainty_cap_ns(static_cast<int64_t>(expected.uncertainty_cap_ns));
    policy.set_keeper_failure_timeout_ms(failure_ms);
    policy.set_release_fence_timeout_ms(fence_ms);
    return policy;
}

// One Keeper with a Synced physical clock fixed at the Visor's base reading; the monotonic clock advances
// one nanosecond per read, so an attempt's bracket is exactly 1 ns and a bracket spanning two attempts is wider.
struct Keeper
{
    explicit Keeper(const std::string& endpoint, uint32_t failure_ms = 15000, uint32_t fence_ms = 2000)
    {
        rig.clock->setPhysical(5'000 * kMs);
        client = std::make_unique<keeper::ClusterClient>(
                grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()),
                keeper::ClusterClient::Options{"self",
                                               "keeper-instance",
                                               "self:1",
                                               std::chrono::milliseconds(500),
                                               "",
                                               failure_ms,
                                               fence_ms,
                                               rig.clock,
                                               [this] { return ++monotonic; }},
                *rig.journal,
                membership,
                watcher);
    }
    std::optional<ClockAuditEntry> entry(const std::string& replica)
    {
        for(auto& e: client->clockAudit())
            if(e.key.replica_id == replica)
                return e;
        return std::nullopt;
    }
    test::RamRig rig;
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher{*rig.journal, "self", nullptr, false};
    std::atomic<int64_t> monotonic{1'000};
    std::unique_ptr<keeper::ClusterClient> client;
};

// Everything the Keeper decides for one append and read script, in comparable form.
std::string script(test::RamRig& rig, uint64_t first_sequence)
{
    std::ostringstream out;
    auto reading = rig.clock->now();
    out << "status=" << static_cast<int>(reading->status) << " bound=" << reading->uncertainty_ns.value_or(0) << ";";
    for(uint64_t sequence = first_sequence; sequence < first_sequence + 3; ++sequence)
    {
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "event " + std::to_string(sequence);
        item.physical = TimeReading{5'000 * kMs + static_cast<int64_t>(sequence), 15, ClockStatus::Synced};
        auto results = rig.journal->append({1, 7, {item}}, Durability::Accepted);
        for(const auto& r: *results)
            out << "append=" << r.status.code() << "/" << r.hlc.physical_ns << "." << r.hlc.logical << "/"
                << static_cast<int>(r.achieved) << ";";
    }
    auto events = rig.journal->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    for(const auto& event: *events) out << "event=" << event.hlc.physical_ns << "." << event.hlc.logical << ";";
    auto view = rig.journal->sealedView(1);
    out << "sealed=" << view->sealed.physical_ns << "." << view->sealed.logical << ";";
    for(const auto& frontier: view->frontiers)
        out << "frontier=" << frontier.writer_id << ":" << frontier.frontier.physical_ns << "."
            << frontier.frontier.logical << ";";
    auto physical = rig.journal->physicalFrontier(1);
    out << "physical=" << (physical.ok() ? *physical : -1) << ";";
    return out.str();
}

} // namespace

// B45 gate 6: an alarm is logged once while the Keeper keeps serving, and the same appends and reads give the
// same ClockStatus, acceptance, HLC and frontiers as on a Keeper whose audit passes.
TEST(KeeperClockAudit, AlarmIsObservationalOnly)
{
    FakeVisor far, near;
    far.offset_ns = 50 * kMs;
    Server far_server(far), near_server(near);
    Keeper alarmed(far_server.endpoint()), passing(near_server.endpoint());
    CapturedLog log;

    const auto before = script(alarmed.rig, 1);
    EXPECT_EQ(before, script(passing.rig, 1));

    for(int beat = 0; beat < 2; ++beat)
    {
        ASSERT_TRUE(alarmed.client->heartbeatNow().ok());
        ASSERT_TRUE(passing.client->heartbeatNow().ok());
    }
    EXPECT_EQ(log.count("clock audit alarm: keeper=self/keeper-instance visor=1/visor-a offset_ns=50000000"), 1u);
    EXPECT_EQ(log.count("clock audit ok: keeper=self/keeper-instance visor=1/visor-a"), 1u);
    auto alarm = alarmed.entry("1");
    ASSERT_TRUE(alarm);
    EXPECT_EQ(alarm->state, ClockAuditState::Alarm);
    EXPECT_EQ(alarm->violations, 2u);
    EXPECT_EQ(alarm->last->offset_ns, 50 * kMs);
    EXPECT_EQ(passing.entry("1")->state, ClockAuditState::Ok);

    EXPECT_EQ(script(alarmed.rig, 4), script(passing.rig, 4));
    EXPECT_EQ(alarmed.rig.clock->now()->status, ClockStatus::Synced);

    // Recovery clears the alarm state and keeps the violation count.
    far.set(0);
    ASSERT_TRUE(alarmed.client->heartbeatNow().ok());
    EXPECT_EQ(log.count("clock audit ok: keeper=self/keeper-instance visor=1/visor-a"), 2u);
    EXPECT_EQ(alarmed.entry("1")->state, ClockAuditState::Ok);
    EXPECT_EQ(alarmed.entry("1")->violations, 2u);
}

TEST(KeeperClockAudit, ForwardedReplyUpdatesTheGeneratingReplica)
{
    // The Keeper is attached to follower "2", which forwards to leader "1"; the reply names the leader.
    FakeVisor follower;
    follower.replica_id = "1";
    follower.instance = "leader";
    Server server(follower);
    Keeper keeper(server.endpoint());
    ASSERT_TRUE(keeper.client->heartbeatNow().ok());
    auto leader = keeper.entry("1");
    ASSERT_TRUE(leader);
    EXPECT_EQ(leader->key.instance, "leader");
    EXPECT_EQ(leader->state, ClockAuditState::Ok);
    EXPECT_FALSE(keeper.entry("2"));
    EXPECT_EQ(keeper.client->clockAudit().size(), 1u);
}

TEST(KeeperClockAudit, FailedAttemptThenAnotherReplicaUsesTheSuccessfulBracket)
{
    FakeVisor down, up;
    down.replica_id = "1";
    up.replica_id = "2";
    up.offset_ns = 3;
    Server down_server(down), up_server(up);
    down.replicas = {up_server.endpoint()};
    Keeper keeper(down_server.endpoint());
    ASSERT_TRUE(keeper.client->registerNow().ok());
    {
        std::lock_guard lock(down.mutex);
        down.fail = true;
    }
    ASSERT_TRUE(keeper.client->heartbeatNow().ok());
    auto entry = keeper.entry("2");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->state, ClockAuditState::Ok);
    // One monotonic step: the bracket of the successful attempt alone, not of the failed one before it.
    EXPECT_EQ(entry->last->rtt_ns, 1);
    EXPECT_EQ(entry->last->offset_ns, 3);
    EXPECT_EQ(keeper.entry("1")->last->rtt_ns, 1);
}

TEST(KeeperClockAudit, MembershipChangeEvictsDepartedReplicas)
{
    FakeVisor visor;
    visor.replicas = {"a:1", "b:1"};
    Server server(visor);
    Keeper keeper(server.endpoint());
    ASSERT_TRUE(keeper.client->registerNow().ok());
    {
        std::lock_guard lock(visor.mutex);
        visor.replica_id = "2";
    }
    ASSERT_TRUE(keeper.client->heartbeatNow().ok());
    EXPECT_EQ(keeper.client->clockAudit().size(), 2u);
    {
        std::lock_guard lock(visor.mutex);
        visor.replicas = {"b:1", "c:1"};
    }
    ASSERT_TRUE(keeper.client->registerNow().ok());
    EXPECT_FALSE(keeper.entry("1"));
    EXPECT_TRUE(keeper.entry("2"));
}

TEST(KeeperClockAudit, UnsyncedKeeperClockIsCoverageLostNotAViolation)
{
    FakeVisor visor;
    Server server(visor);
    Keeper keeper(server.endpoint());
    CapturedLog log;
    ASSERT_TRUE(keeper.client->heartbeatNow().ok());
    keeper.rig.clock->setStatus(ClockStatus::Unsynced);
    visor.set(50 * kMs);
    ASSERT_TRUE(keeper.client->heartbeatNow().ok());
    auto entry = keeper.entry("1");
    EXPECT_EQ(entry->state, ClockAuditState::Inconclusive);
    EXPECT_EQ(entry->reason, ClockAuditReason::Unsynced);
    EXPECT_EQ(entry->violations, 0u);
    EXPECT_EQ(log.count("clock audit coverage lost: keeper=self/keeper-instance visor=1/visor-a reason=clock unsynced"),
              1u);
    EXPECT_EQ(keeper.rig.clock->now()->status, ClockStatus::Unsynced);
}

TEST(KeeperClockAudit, VisorTimeoutsReplaceTheConfigDeadline)
{
    FakeVisor visor;
    visor.policy = policyWith(3000, 1000);
    Server server(visor);
    Keeper keeper(server.endpoint(), 15000, 2000);
    EXPECT_EQ(keeper.client->heartbeatDeadline(), keeper::heartbeatDeadline(500ms, 15000, 2000));
    EXPECT_EQ(keeper.client->heartbeatDeadline(), 1000ms);
    CapturedLog log;
    ASSERT_TRUE(keeper.client->registerNow().ok());
    EXPECT_EQ(keeper.client->heartbeatDeadline(), keeper::heartbeatDeadline(500ms, 3000, 1000));
    EXPECT_EQ(keeper.client->heartbeatDeadline(), 500ms);
    ASSERT_TRUE(keeper.client->registerNow().ok());
    EXPECT_EQ(log.count("adopting the Visor's keeper_failure_timeout_ms 3000 (was 15000)"), 1u);
}

TEST(KeeperClockAudit, PolicyWithoutTimeoutsKeepsTheConfigValues)
{
    FakeVisor visor;
    visor.policy = policyWith(0, 0);
    Server server(visor);
    Keeper keeper(server.endpoint(), 15000, 2000);
    ASSERT_TRUE(keeper.client->registerNow().ok());
    EXPECT_EQ(keeper.client->heartbeatDeadline(), 1000ms);
    {
        std::lock_guard lock(visor.mutex);
        visor.policy.reset();
    }
    ASSERT_TRUE(keeper.client->registerNow().ok());
    EXPECT_EQ(keeper.client->heartbeatDeadline(), 1000ms);
}

TEST(KeeperClockAudit, VisorTimeoutsThatLeaveNoDeadlineAreRefused)
{
    FakeVisor visor;
    visor.policy = policyWith(600, 2000);
    Server server(visor);
    Keeper keeper(server.endpoint(), 15000, 2000);
    EXPECT_TRUE(absl::IsFailedPrecondition(keeper.client->registerNow()));
    EXPECT_EQ(keeper.client->heartbeatDeadline(), 1000ms);
}

TEST(KeeperPolicy, ConfiguredSkewMismatchRefusesRegister)
{
    FakeVisor visor;
    visor.policy = policyWith(15000, 2000);
    Server server(visor);
    test::RamRig rig({.require_catalog_policy = true});
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient::Options options{"self", "instance", "self:1"};
    options.causal_floor_skew_limit_ns = 59'000'000'000;
    keeper::ClusterClient client(grpc::CreateChannel(server.endpoint(), grpc::InsecureChannelCredentials()),
                                 options,
                                 *rig.journal,
                                 membership,
                                 watcher);
    const auto status = client.registerNow();
    EXPECT_TRUE(absl::IsFailedPrecondition(status));
    EXPECT_NE(status.message().find("causal_floor_skew_limit_ns=59000000000"), std::string::npos);
    EXPECT_NE(status.message().find("Catalog skew_limit_ns=60000000000"), std::string::npos);
    EXPECT_FALSE(client.registered());
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    EXPECT_TRUE(absl::IsUnavailable(rig.journal->append({1, 7, {item}}, Durability::Accepted).status()));
}

TEST(KeeperPolicy, ConfiguredReserveMismatchRefusesRegister)
{
    FakeVisor visor;
    visor.policy = policyWith(15000, 2000);
    Server server(visor);
    test::RamRig rig({.require_catalog_policy = true});
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    for(uint32_t reserve_ms: {999u, UINT32_MAX})
    {
        keeper::ClusterClient::Options options{"self", "instance", "self:1"};
        options.reserve_ahead_ms = reserve_ms;
        keeper::ClusterClient client(grpc::CreateChannel(server.endpoint(), grpc::InsecureChannelCredentials()),
                                     options,
                                     *rig.journal,
                                     membership,
                                     watcher);
        const auto status = client.registerNow();
        EXPECT_TRUE(absl::IsFailedPrecondition(status));
        EXPECT_NE(status.message().find("reserve_ahead_ms=" + std::to_string(reserve_ms)), std::string::npos);
        EXPECT_NE(status.message().find("Catalog reserve_ahead_ns=1000000000"), std::string::npos);
        EXPECT_NE(status.message().find("S=60000000000, D=61000000000"), std::string::npos);
        EXPECT_FALSE(client.registered());
    }
}

TEST(KeeperPolicy, MatchingConfigRegisters)
{
    FakeVisor visor;
    visor.policy = policyWith(15000, 2000);
    Server server(visor);
    test::RamRig rig({.require_catalog_policy = true});
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient client(grpc::CreateChannel(server.endpoint(), grpc::InsecureChannelCredentials()),
                                 {"self", "instance", "self:1"},
                                 *rig.journal,
                                 membership,
                                 watcher);
    EXPECT_TRUE(client.registerNow().ok());
    EXPECT_TRUE(client.registered());
}

TEST(KeeperPolicy, RegisteredCatalogSkewControlsCausalFloorBoundary)
{
    FakeVisor visor;
    visor.policy = policyWith(15000, 2000);
    Server server(visor);
    test::RamRig rig({.physical_policy = {.skew_limit_ns = 1}, .require_catalog_policy = true});
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient client(grpc::CreateChannel(server.endpoint(), grpc::InsecureChannelCredentials()),
                                 {"self", "instance", "self:1"},
                                 *rig.journal,
                                 membership,
                                 watcher);
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    item.physical = {100, 0, ClockStatus::Synced};
    item.causal_floor = {100 + visor.policy->skew_limit_ns() - 1, 0};
    EXPECT_TRUE(absl::IsUnavailable(rig.journal->append({1, 7, {item}}, Durability::Accepted).status()));
    ASSERT_TRUE(client.registerNow().ok());
    auto inside = rig.journal->append({1, 7, {item}}, Durability::Accepted);
    ASSERT_TRUE(inside.ok()) << inside.status();
    ASSERT_EQ(inside->size(), 1u);
    EXPECT_TRUE(inside->front().status.ok()) << inside->front().status;
    item.sequence = 2;
    item.causal_floor.physical_ns = 100 + visor.policy->skew_limit_ns() + 1;
    auto outside = rig.journal->append({1, 7, {item}}, Durability::Accepted);
    ASSERT_TRUE(outside.ok()) << outside.status();
    ASSERT_EQ(outside->size(), 1u);
    EXPECT_TRUE(absl::IsInvalidArgument(outside->front().status));
    auto events = rig.journal->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
}

TEST(KeeperPolicy, MissingCatalogPolicyLeavesAdmissionClosed)
{
    FakeVisor visor;
    Server server(visor);
    test::RamRig rig({.require_catalog_policy = true});
    keeper::ConfigMembership membership;
    keeper::AcquisitionWatcher watcher(*rig.journal, "self", nullptr, false);
    keeper::ClusterClient client(grpc::CreateChannel(server.endpoint(), grpc::InsecureChannelCredentials()),
                                 {"self", "instance", "self:1"},
                                 *rig.journal,
                                 membership,
                                 watcher);
    const auto status = client.registerNow();
    EXPECT_TRUE(absl::IsFailedPrecondition(status));
    EXPECT_NE(status.message().find("omitted the Catalog physical policy"), std::string::npos);
    EXPECT_FALSE(client.registered());
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    EXPECT_TRUE(absl::IsUnavailable(rig.journal->append({1, 7, {item}}, Durability::Accepted).status()));
}

} // namespace chronolog
