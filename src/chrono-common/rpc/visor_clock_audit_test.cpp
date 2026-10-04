// B45 Part 1 gates for VisorClockAudit, shared by the Grapher and the Player: the clock audit on Register and
// Heartbeat replies is observational.
#include "rpc/VisorClockAudit.h"
#include "clock/FakeClock.h"

#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace chronolog
{
namespace
{
namespace wire = internal::v1;

constexpr int64_t kNow = 1'700'000'000'000'000'000;
constexpr uint64_t kBound = 1'000'000;

class CapturingSink final: public absl::LogSink
{
public:
    CapturingSink() { absl::AddLogSink(this); }
    ~CapturingSink() override { absl::RemoveLogSink(this); }
    void Send(const absl::LogEntry& entry) override
    {
        std::lock_guard lock(mu_);
        messages_.emplace_back(entry.text_message());
    }
    size_t count(const std::string& needle)
    {
        std::lock_guard lock(mu_);
        return std::count_if(messages_.begin(),
                             messages_.end(),
                             [&](const std::string& text) { return text.find(needle) != std::string::npos; });
    }

private:
    std::mutex mu_;
    std::vector<std::string> messages_;
};

std::shared_ptr<FakeClock> syncedClock()
{
    auto clock = std::make_shared<FakeClock>(kNow, kBound);
    clock->setStatus(ClockStatus::Synced);
    return clock;
}

template <class Response>
void stamp(Response& response, int64_t physical_ns, const std::string& replica, v1::ClockStatus status)
{
    auto* physical = response.mutable_physical();
    physical->set_physical_ns(physical_ns);
    physical->set_status(status);
    if(status == v1::CLOCK_STATUS_SYNCED)
        physical->set_uncertainty_ns(kBound);
    if(!replica.empty())
    {
        response.mutable_clock_responder()->set_replica_id(replica);
        response.mutable_clock_responder()->set_instance(replica + "-1");
    }
}

std::optional<ClockAuditState> stateOf(const VisorClockAudit& audit, const std::string& replica)
{
    for(const auto& entry: audit.entries())
        if(entry.key.replica_id == replica)
            return entry.state;
    return std::nullopt;
}

TEST(GrapherClockAudit, AnonymousUnsyncedOrSteppedReplyIsInconclusive)
{
    CapturingSink sink;
    auto clock = syncedClock();
    std::atomic<int64_t> mono{1'000};
    VisorClockAudit audit("grapher", "grapher-1/i1", clock, [&] { return mono.load(); });

    wire::HeartbeatResponse anonymous;
    stamp(anonymous, kNow, "", v1::CLOCK_STATUS_SYNCED);
    auto decision = audit.finish(audit.begin(), grpc::Status::OK, anonymous);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::MissingIdentity);
    EXPECT_TRUE(audit.entries().empty());
    // A repeated anonymous reply is the same coverage loss, not a new line.
    audit.finish(audit.begin(), grpc::Status::OK, anonymous);

    wire::RegisterResponse unsynced;
    stamp(unsynced, kNow, "visor-a", v1::CLOCK_STATUS_UNSYNCED);
    decision = audit.finish(audit.begin(), grpc::Status::OK, unsynced);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::Unsynced);
    EXPECT_EQ(stateOf(audit, "visor-a"), ClockAuditState::Inconclusive);

    wire::HeartbeatResponse unspecified;
    stamp(unspecified, kNow, "visor-a", v1::CLOCK_STATUS_UNSPECIFIED);
    decision = audit.finish(audit.begin(), grpc::Status::OK, unspecified);
    EXPECT_EQ(decision.reason, ClockAuditReason::Unavailable);

    // A Synced status without its bound is malformed, never a zero bound.
    wire::HeartbeatResponse unbounded;
    stamp(unbounded, kNow, "visor-a", v1::CLOCK_STATUS_SYNCED);
    unbounded.mutable_physical()->clear_uncertainty_ns();
    decision = audit.finish(audit.begin(), grpc::Status::OK, unbounded);
    EXPECT_EQ(decision.reason, ClockAuditReason::MissingBound);

    // The local clock steps one second inside the bracket while the monotonic clock stands still.
    wire::HeartbeatResponse synced;
    stamp(synced, kNow, "visor-a", v1::CLOCK_STATUS_SYNCED);
    const auto bracket = audit.begin();
    clock->setPhysical(kNow + 1'000'000'000);
    decision = audit.finish(bracket, grpc::Status::OK, synced);
    EXPECT_EQ(decision.reason, ClockAuditReason::Discontinuity);
    EXPECT_EQ(stateOf(audit, "visor-a"), ClockAuditState::Inconclusive);

    audit.report();
    EXPECT_EQ(sink.count("clock audit coverage lost role=grapher identity=grapher-1/i1 replica= instance= "
                         "reason=missing_identity"),
              1u);
    EXPECT_EQ(sink.count("replica=visor-a instance=visor-a-1 reason=unsynced"), 1u);
    EXPECT_EQ(sink.count("clock audit OK"), 0u);
    EXPECT_EQ(sink.count("clock audit ALARM"), 0u);
}

TEST(GrapherClockAudit, FailedAttemptThenAnotherReplicaUsesTheSuccessfulBracket)
{
    auto clock = syncedClock();
    std::atomic<int64_t> mono{1'000};
    VisorClockAudit audit("grapher", "grapher-1/i1", clock, [&] { return mono.load(); });

    wire::HeartbeatResponse failed;
    const auto first = audit.begin();
    mono += 100'000'000;
    clock->setPhysical(kNow + 100'000'000);
    auto decision = audit.finish(first, grpc::Status(grpc::StatusCode::UNAVAILABLE, "down"), failed);
    EXPECT_EQ(decision.reason, ClockAuditReason::TransportFailure);

    // Against the failed bracket's t0 this reply would be 50 ms ahead and alarm.
    wire::HeartbeatResponse reply;
    stamp(reply, kNow + 100'000'500, "visor-b", v1::CLOCK_STATUS_SYNCED);
    decision = audit.finish(audit.begin(), grpc::Status::OK, reply);
    ASSERT_EQ(decision.state, ClockAuditState::Ok);
    ASSERT_TRUE(decision.observation);
    EXPECT_EQ(decision.observation->offset_ns, 500);
    EXPECT_EQ(decision.observation->rtt_ns, 0);
    EXPECT_EQ(stateOf(audit, "visor-b"), ClockAuditState::Ok);
    EXPECT_EQ(audit.entries().size(), 1u);
}

TEST(GrapherClockAudit, ReplicasLeavingTheVisorMembershipAreEvicted)
{
    VisorClockAudit audit("grapher", "grapher-1/i1", syncedClock(), [] { return int64_t{1'000}; });
    for(const std::string replica: {"visor-a:50051", "visor-b:50051"})
    {
        wire::HeartbeatResponse reply;
        stamp(reply, kNow, replica, v1::CLOCK_STATUS_SYNCED);
        audit.finish(audit.begin(), grpc::Status::OK, reply);
    }
    google::protobuf::RepeatedPtrField<std::string> members;
    audit.retain(members);
    EXPECT_EQ(audit.entries().size(), 2u);
    *members.Add() = "visor-a:50051";
    audit.retain(members);
    ASSERT_EQ(audit.entries().size(), 1u);
    EXPECT_EQ(audit.entries()[0].key.replica_id, "visor-a:50051");
}

TEST(PlayerClockAudit, AnonymousOrUnsyncedReplyIsInconclusive)
{
    CapturingSink sink;
    VisorClockAudit audit("player", "player-1/i1", syncedClock(), [] { return int64_t{1'000}; });

    wire::RegisterResponse anonymous;
    stamp(anonymous, kNow, "", v1::CLOCK_STATUS_SYNCED);
    auto decision = audit.finish(audit.begin(), grpc::Status::OK, anonymous);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::MissingIdentity);
    // A repeated anonymous reply is the same coverage loss, not a new line.
    audit.finish(audit.begin(), grpc::Status::OK, anonymous);

    wire::HeartbeatResponse unsynced;
    stamp(unsynced, kNow, "visor-a", v1::CLOCK_STATUS_UNSYNCED);
    decision = audit.finish(audit.begin(), grpc::Status::OK, unsynced);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::Unsynced);
    EXPECT_EQ(stateOf(audit, "visor-a"), ClockAuditState::Inconclusive);

    audit.report();
    EXPECT_EQ(sink.count("clock audit coverage lost role=player identity=player-1/i1 replica= instance= "
                         "reason=missing_identity"),
              1u);
    EXPECT_EQ(sink.count("replica=visor-a instance=visor-a-1 reason=unsynced"), 1u);
    EXPECT_EQ(sink.count("clock audit OK"), 0u);
}

TEST(PlayerClockAudit, FailedAttemptThenAnotherReplicaUsesTheSuccessfulBracket)
{
    auto clock = syncedClock();
    std::atomic<int64_t> mono{1'000};
    VisorClockAudit audit("player", "player-1/i1", clock, [&] { return mono.load(); });

    wire::HeartbeatResponse failed;
    const auto first = audit.begin();
    mono += 100'000'000;
    clock->setPhysical(kNow + 100'000'000);
    auto decision = audit.finish(first, grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "slow"), failed);
    EXPECT_EQ(decision.reason, ClockAuditReason::TransportFailure);

    // Against the failed bracket's t0 this reply would be 50 ms ahead and alarm.
    wire::HeartbeatResponse reply;
    stamp(reply, kNow + 100'000'500, "visor-b", v1::CLOCK_STATUS_SYNCED);
    decision = audit.finish(audit.begin(), grpc::Status::OK, reply);
    ASSERT_EQ(decision.state, ClockAuditState::Ok);
    ASSERT_TRUE(decision.observation);
    EXPECT_EQ(decision.observation->offset_ns, 500);
    EXPECT_EQ(stateOf(audit, "visor-b"), ClockAuditState::Ok);
    EXPECT_EQ(audit.entries().size(), 1u);
}

} // namespace
} // namespace chronolog
