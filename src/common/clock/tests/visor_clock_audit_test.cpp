// B45 Part 1 gates for the VisorClockAudit core shared by the Grapher and the Player. The wire reading of a
// Register or Heartbeat reply is gated in each service's own tests.
#include "common/clock/VisorClockAudit.h"
#include "common/clock/FakeClock.h"

#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace chronolog
{
namespace
{

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

TimeReading synced(int64_t physical_ns) { return {physical_ns, kBound, ClockStatus::Synced}; }

ClockResponderKey replica(const std::string& id) { return {id, id + "-1"}; }

std::optional<ClockAuditState> stateOf(const VisorClockAudit& audit, const std::string& replica)
{
    for(const auto& entry: audit.entries())
        if(entry.key.replica_id == replica)
            return entry.state;
    return std::nullopt;
}

TEST(VisorClockAudit, LogLinesNameTheRole)
{
    for(const std::string role: {"grapher", "player"})
    {
        CapturingSink sink;
        std::atomic<int64_t> mono{1'000};
        VisorClockAudit audit(role, role + "-1/i1", syncedClock(), [&] { return mono.load(); });
        audit.record(audit.begin(), true, synced(kNow), replica("visor-a"));
        // Offset 10 ms against a threshold of local 1 ms + Visor 1 ms + RTT/2 0.
        audit.record(audit.begin(), true, synced(kNow + 10'000'000), replica("visor-a"));
        audit.report();
        audit.record(audit.begin(), true, synced(kNow), replica("visor-a"));
        audit.record(audit.begin(), true, synced(kNow), {});
        audit.report();
        EXPECT_EQ(sink.count("clock audit ALARM role=" + role + " identity=" + role +
                             "-1/i1 replica=visor-a instance=visor-a-1 offset_ns=10000000 local_bound_ns=1000000 "
                             "visor_bound_ns=1000000 rtt_ns=0 threshold_ns=2000000"),
                  1u);
        EXPECT_EQ(sink.count("clock audit OK role=" + role + " identity=" + role +
                             "-1/i1 replica=visor-a instance=visor-a-1 offset_ns=0"),
                  1u);
        EXPECT_EQ(sink.count("clock audit coverage lost role=" + role + " identity=" + role +
                             "-1/i1 replica= instance= reason=missing_identity"),
                  1u);
    }
}

TEST(VisorClockAudit, FailedAttemptThenAnotherReplicaUsesTheSuccessfulBracket)
{
    auto clock = syncedClock();
    std::atomic<int64_t> mono{1'000};
    VisorClockAudit audit("grapher", "grapher-1/i1", clock, [&] { return mono.load(); });

    const auto first = audit.begin();
    mono += 100'000'000;
    clock->setPhysical(kNow + 100'000'000);
    auto decision = audit.record(first, false, std::nullopt, {});
    EXPECT_EQ(decision.reason, ClockAuditReason::TransportFailure);

    // Against the failed bracket's t0 this reply would be 50 ms ahead and alarm.
    decision = audit.record(audit.begin(), true, synced(kNow + 100'000'500), replica("visor-b"));
    ASSERT_EQ(decision.state, ClockAuditState::Ok);
    ASSERT_TRUE(decision.observation);
    EXPECT_EQ(decision.observation->offset_ns, 500);
    EXPECT_EQ(decision.observation->rtt_ns, 0);
    EXPECT_EQ(stateOf(audit, "visor-b"), ClockAuditState::Ok);
    EXPECT_EQ(audit.entries().size(), 1u);
}

TEST(VisorClockAudit, ReplicasLeavingTheVisorMembershipAreEvicted)
{
    VisorClockAudit audit("grapher", "grapher-1/i1", syncedClock(), [] { return int64_t{1'000}; });
    for(const std::string id: {"visor-a:50051", "visor-b:50051"})
        audit.record(audit.begin(), true, synced(kNow), replica(id));
    audit.retain({});
    EXPECT_EQ(audit.entries().size(), 2u);
    audit.retain({"visor-a:50051"});
    ASSERT_EQ(audit.entries().size(), 1u);
    EXPECT_EQ(audit.entries()[0].key.replica_id, "visor-a:50051");
}

} // namespace
} // namespace chronolog
