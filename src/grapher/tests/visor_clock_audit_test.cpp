// B45 Part 1 gates for the Grapher: the clock audit on Register and Heartbeat is observational.
#include "grapher/ClusterWorker.h"
#include "common/clock/FakeClock.h"
#include "common/rpc/Channel.h"

#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace chronolog::grapher
{
namespace
{
using namespace std::chrono_literals;
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

// Answers Register and Heartbeat with a Visor reading kNow + offset from replica "visor-a".
class FakeVisor final: public wire::Cluster::Service
{
public:
    grpc::Status Register(grpc::ServerContext*, const wire::RegisterRequest*, wire::RegisterResponse* r) override
    {
        ++registers;
        stamp(*r, kNow + offset, "visor-a", v1::CLOCK_STATUS_SYNCED);
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatRequest*, wire::HeartbeatResponse* r) override
    {
        stamp(*r, kNow + offset, "visor-a", v1::CLOCK_STATUS_SYNCED);
        {
            std::lock_guard lock(mu);
            ++heartbeats;
        }
        cv.notify_all();
        return grpc::Status::OK;
    }
    bool waitHeartbeats(int count)
    {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, 10s, [&] { return heartbeats >= count; });
    }
    int seen()
    {
        std::lock_guard lock(mu);
        return heartbeats;
    }

    std::atomic<int64_t> offset{0};
    std::atomic<int> registers{0};

private:
    std::mutex mu;
    std::condition_variable cv;
    int heartbeats{0};
};

std::optional<ClockAuditState> stateOf(const VisorClockAudit& audit, const std::string& replica)
{
    for(const auto& entry: audit.entries())
        if(entry.key.replica_id == replica)
            return entry.state;
    return std::nullopt;
}

bool waitState(const VisorClockAudit& audit, ClockAuditState state)
{
    for(int i = 0; i < 1000; ++i)
    {
        if(stateOf(audit, "visor-a") == state)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

TEST(GrapherClockAudit, AlarmIsLoggedOnceWhileTheGrapherKeepsHeartbeating)
{
    CapturingSink sink;
    FakeVisor visor;
    grpc::ServerBuilder builder;
    builder.RegisterService(&visor);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = wire::Cluster::NewStub(server->InProcessChannel(grpc::ChannelArguments()));
    VisorClockAudit audit("grapher", "grapher-1/i1", syncedClock(), [] { return int64_t{5'000'000'000}; });
    ClusterWorkerOptions options;
    options.process_id = "grapher-1";
    options.instance = "i1";
    options.heartbeat_interval = 1ms;
    std::jthread worker([&](std::stop_token stop) { runClusterWorker(stop, *stub, audit, options); });

    ASSERT_TRUE(visor.waitHeartbeats(2));
    ASSERT_TRUE(waitState(audit, ClockAuditState::Ok));
    // Offset 10 ms against a threshold of local 1 ms + Visor 1 ms + RTT/2 0.
    visor.offset = 10'000'000;
    ASSERT_TRUE(waitState(audit, ClockAuditState::Alarm));
    const int after_alarm = visor.seen();
    ASSERT_TRUE(visor.waitHeartbeats(after_alarm + 3));
    EXPECT_EQ(visor.registers.load(), 1);
    visor.offset = 0;
    ASSERT_TRUE(waitState(audit, ClockAuditState::Ok));
    worker.request_stop();
    worker.join();
    server->Shutdown(std::chrono::system_clock::now() + 1s);

    EXPECT_EQ(sink.count("clock audit ALARM role=grapher identity=grapher-1/i1 replica=visor-a instance=visor-a-1 "
                         "offset_ns=10000000 local_bound_ns=1000000 visor_bound_ns=1000000 rtt_ns=0 "
                         "threshold_ns=2000000"),
              1u);
    EXPECT_EQ(sink.count("clock audit OK role=grapher"), 1u);
    EXPECT_EQ(sink.count("clock audit coverage lost"), 0u);
    const auto entries = audit.entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_GE(entries[0].violations, 1u);
}

TEST(GrapherClockAudit, AnonymousUnsyncedOrSteppedReplyIsInconclusive)
{
    CapturingSink sink;
    auto clock = syncedClock();
    std::atomic<int64_t> mono{1'000};
    VisorClockAudit audit("grapher", "grapher-1/i1", clock, [&] { return mono.load(); });

    wire::HeartbeatResponse anonymous;
    stamp(anonymous, kNow, "", v1::CLOCK_STATUS_SYNCED);
    auto decision = auditVisorReply(audit, audit.begin(), grpc::Status::OK, anonymous);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::MissingIdentity);
    EXPECT_TRUE(audit.entries().empty());
    // A repeated anonymous reply is the same coverage loss, not a new line.
    auditVisorReply(audit, audit.begin(), grpc::Status::OK, anonymous);

    wire::RegisterResponse unsynced;
    stamp(unsynced, kNow, "visor-a", v1::CLOCK_STATUS_UNSYNCED);
    decision = auditVisorReply(audit, audit.begin(), grpc::Status::OK, unsynced);
    EXPECT_EQ(decision.state, ClockAuditState::Inconclusive);
    EXPECT_EQ(decision.reason, ClockAuditReason::Unsynced);
    EXPECT_EQ(stateOf(audit, "visor-a"), ClockAuditState::Inconclusive);

    wire::HeartbeatResponse unspecified;
    stamp(unspecified, kNow, "visor-a", v1::CLOCK_STATUS_UNSPECIFIED);
    decision = auditVisorReply(audit, audit.begin(), grpc::Status::OK, unspecified);
    EXPECT_EQ(decision.reason, ClockAuditReason::Unavailable);

    // A Synced status without its bound is malformed, never a zero bound.
    wire::HeartbeatResponse unbounded;
    stamp(unbounded, kNow, "visor-a", v1::CLOCK_STATUS_SYNCED);
    unbounded.mutable_physical()->clear_uncertainty_ns();
    decision = auditVisorReply(audit, audit.begin(), grpc::Status::OK, unbounded);
    EXPECT_EQ(decision.reason, ClockAuditReason::MissingBound);

    // The local clock steps one second inside the bracket while the monotonic clock stands still.
    wire::HeartbeatResponse synced;
    stamp(synced, kNow, "visor-a", v1::CLOCK_STATUS_SYNCED);
    const auto bracket = audit.begin();
    clock->setPhysical(kNow + 1'000'000'000);
    decision = auditVisorReply(audit, bracket, grpc::Status::OK, synced);
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

} // namespace
} // namespace chronolog::grapher
