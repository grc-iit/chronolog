#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>

#include "chronolog/membership.h"
#include "membership/Topology.h"

namespace chronolog::visor
{

// Membership with a fixed route from configuration. Epochs are static until M8
// but are still carried and validated on every call (I4.3).
class StaticRouteMembership final: public Membership
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    // `exists` tells whether a story is live; it is injected from the Catalog store.
    StaticRouteMembership(Topology topology,
                          Epoch epoch,
                          std::function<bool(StoryId)> exists,
                          std::chrono::milliseconds heartbeat_timeout,
                          std::function<TimePoint()> now = &std::chrono::steady_clock::now);

    absl::StatusOr<Route> route(StoryId id) const override;
    absl::Status validateEpoch(StoryId id, Epoch epoch) const override;
    absl::Status registerProcess(Process process) override;
    absl::Status heartbeat(std::string id, std::string instance, uint64_t applied_revision = 0) override;

    // Keeper for a writer in an epoch. Same function the Catalog uses.
    absl::StatusOr<KeeperRef> assignKeeper(uint64_t writer_id, Epoch epoch) const;

    // Blocks until the current instance of the Keeper registered as `process_id` reports an
    // applied revision >= `revision`, or `timeout` passes. Used to confirm a Release
    // fence. Returns false on timeout and when no such Keeper is registered.
    bool waitApplied(const std::string& process_id, uint64_t revision, std::chrono::milliseconds timeout) const;

    // True when the process registered and its latest heartbeat is newer than the timeout.
    bool alive(const std::string& id) const;
    std::optional<Process> process(const std::string& id) const;

private:
    struct Entry
    {
        Process process;
        TimePoint last_heartbeat;
        // Highest contiguous acquisition revision the process reports as applied.
        uint64_t applied_revision{};
    };

    const Topology topology_;
    const Epoch epoch_;
    const std::function<bool(StoryId)> exists_;
    const std::chrono::milliseconds heartbeat_timeout_;
    const std::function<TimePoint()> now_;
    mutable std::shared_mutex mutex_;
    mutable std::condition_variable_any applied_cv_;
    std::map<std::string, Entry> processes_;
};

} // namespace chronolog::visor
