#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chronolog/clock.h"
#include "common/clock/ClockAudit.h"

namespace chronolog
{

// Observational audit of a process's physical clock against the Visor readings on its Register and
// Heartbeat replies (B45 Part 1, I8.3). Nothing here changes ClockStatus or anything the process serves. Each
// service's adapter reads the reply off the wire and calls record().
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

    // Closes the bracket and records the attempt: replied is false on a transport failure, visor is the reply's
    // physical reading as received, responder the replica that generated it.
    ClockAuditDecision
    record(const Bracket& bracket, bool replied, std::optional<TimeReading> visor, ClockResponderKey responder);

    // Drops replicas outside the Visor membership; an empty list (static Visor) keeps every entry.
    void retain(const std::vector<std::string>& replicas);

    // Logs pending transitions and stale coverage on the calling worker thread.
    void report();

    std::vector<ClockAuditEntry> entries() const;

private:
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
