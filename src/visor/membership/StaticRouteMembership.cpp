#include "visor/membership/StaticRouteMembership.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace chronolog::visor
{

StaticRouteMembership::StaticRouteMembership(Topology topology,
                                             Epoch epoch,
                                             std::function<bool(StoryId)> exists,
                                             std::chrono::milliseconds heartbeat_timeout,
                                             std::function<TimePoint()> now)
    : topology_(std::move(topology))
    , epoch_(epoch)
    , exists_(std::move(exists))
    , heartbeat_timeout_(heartbeat_timeout)
    , now_(std::move(now))
{}

absl::StatusOr<Route> StaticRouteMembership::route(StoryId id) const
{
    if(!exists_(id))
        return absl::NotFoundError("unknown story");
    return topology_.routeFor(epoch_, id);
}

absl::Status StaticRouteMembership::validateEpoch(StoryId id, Epoch epoch) const
{
    if(!exists_(id))
        return absl::NotFoundError("unknown story");
    if(epoch != epoch_)
        return absl::FailedPreconditionError("stale epoch");
    return absl::OkStatus();
}

absl::Status StaticRouteMembership::registerProcess(Process process)
{
    if(process.id.empty() || process.instance.empty() || process.endpoint.empty())
        return absl::InvalidArgumentError("process id, instance and endpoint are required");
    std::unique_lock lock(mutex_);
    auto current = processes_.find(process.id);
    if(current != processes_.end() && current->second.process.instance == process.instance)
    {
        current->second.process = std::move(process);
        return absl::OkStatus();
    }
    // The latest registration wins. Instance strings are opaque, so recency is
    // arrival order, and the previous instance's heartbeats are fenced.
    Entry entry{process, now_(), 0};
    processes_[process.id] = std::move(entry);
    applied_cv_.notify_all();
    return absl::OkStatus();
}

absl::Status StaticRouteMembership::heartbeat(std::string id, std::string instance, uint64_t applied_revision)
{
    if(id.empty() || instance.empty())
        return absl::InvalidArgumentError("process id and instance are required");
    {
        std::unique_lock lock(mutex_);
        auto it = processes_.find(id);
        if(it == processes_.end())
            return absl::NotFoundError("unknown process");
        if(it->second.process.instance != instance)
            return absl::FailedPreconditionError("obsolete process instance");
        it->second.last_heartbeat = now_();
        // A stale report never lowers the applied revision.
        it->second.applied_revision = std::max(it->second.applied_revision, applied_revision);
    }
    applied_cv_.notify_all();
    return absl::OkStatus();
}

bool StaticRouteMembership::waitApplied(const std::string& process_id,
                                        uint64_t revision,
                                        std::chrono::milliseconds timeout) const
{
    std::shared_lock lock(mutex_);
    return applied_cv_.wait_for(lock,
                                std::max(timeout, std::chrono::milliseconds::zero()),
                                [&]
                                {
                                    for(const auto& [id, entry]: processes_)
                                        if(entry.process.role == ProcessRole::Keeper &&
                                           entry.process.id == process_id && entry.applied_revision >= revision)
                                            return true;
                                    return false;
                                });
}

absl::StatusOr<KeeperRef> StaticRouteMembership::assignKeeper(uint64_t writer_id, Epoch epoch) const
{
    return topology_.assignKeeper(writer_id, epoch);
}

bool StaticRouteMembership::alive(const std::string& id) const
{
    std::shared_lock lock(mutex_);
    auto it = processes_.find(id);
    return it != processes_.end() && now_() - it->second.last_heartbeat <= heartbeat_timeout_;
}

std::optional<Process> StaticRouteMembership::process(const std::string& id) const
{
    std::shared_lock lock(mutex_);
    auto it = processes_.find(id);
    if(it == processes_.end())
        return std::nullopt;
    return it->second.process;
}

} // namespace chronolog::visor
