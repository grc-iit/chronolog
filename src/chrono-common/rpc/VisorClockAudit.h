#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/support/status.h>

#include "chronolog/clock.h"
#include "chronolog/internal/v1/internal.pb.h"
#include "clock/ClockAudit.h"

namespace chronolog
{

// Observational audit of a process's physical clock against the Visor readings on its Register and
// Heartbeat replies (B45 Part 1, I8.3). Nothing here changes ClockStatus or anything the process serves.
class VisorClockAudit
{
public:
    using Monotonic = std::function<int64_t()>;

    // role names the process kind in log lines ("grapher", "player"). monotonic defaults to steady_clock; tests
    // inject both clocks.
    VisorClockAudit(std::string role,
                    std::string identity,
                    std::shared_ptr<const Clock> physical,
                    Monotonic monotonic = {},
                    ClockAuditOptions options = {});

    struct Bracket
    {
        TimeReading t0;
        int64_t m0_ns{};
    };
    // Opens the bracket of one RPC attempt.
    Bracket begin() const;

    // Closes the bracket and records the attempt's reply.
    template <class Response>
    ClockAuditDecision finish(const Bracket& bracket, const grpc::Status& status, const Response& response)
    {
        return record(bracket,
                      status.ok(),
                      status.ok() && response.has_physical() ? &response.physical() : nullptr,
                      response.clock_responder());
    }

    // Drops replicas outside the Visor membership; an empty list (static Visor) keeps every entry.
    void retain(const google::protobuf::RepeatedPtrField<std::string>& replicas);

    // Logs pending transitions and stale coverage on the calling worker thread.
    void report();

    std::vector<ClockAuditEntry> entries() const;

private:
    ClockAuditDecision record(const Bracket& bracket,
                              bool replied,
                              const v1::TimeReading* visor,
                              const internal::v1::ClockResponder& responder);
    void log(const ClockAuditTransition& transition, int64_t now_ns);
    bool admit(const std::string& replica, ClockAuditTransition::Kind kind, int64_t now_ns, uint64_t& suppressed);

    std::string role_;
    std::string identity_;
    std::shared_ptr<const Clock> physical_;
    Monotonic monotonic_;
    ClockAudit audit_;
    std::mutex mutex_;
    std::vector<ClockAuditTransition> pending_;
    ClockAuditReason unattributed_{ClockAuditReason::None};
    struct Limit
    {
        int64_t logged_ns{};
        uint64_t suppressed{};
    };
    std::map<std::pair<std::string, ClockAuditTransition::Kind>, Limit> limits_;
};

} // namespace chronolog
