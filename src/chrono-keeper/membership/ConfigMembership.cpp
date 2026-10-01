#include "membership/ConfigMembership.h"

#include <mutex>

namespace chronolog::keeper
{

ConfigMembership::ConfigMembership(const std::vector<StaticRoute>& seed)
{
    for(const auto& entry: seed) routes_[entry.story_id] = entry.route;
}

void ConfigMembership::setRoute(StoryId id, Route route)
{
    std::unique_lock lock(mutex_);
    routes_[id] = std::move(route);
}

absl::StatusOr<Route> ConfigMembership::route(StoryId id) const
{
    std::shared_lock lock(mutex_);
    auto it = routes_.find(id);
    if(it == routes_.end())
        return absl::NotFoundError("unknown story");
    return it->second;
}

absl::Status ConfigMembership::validateEpoch(StoryId id, Epoch epoch) const
{
    std::shared_lock lock(mutex_);
    auto it = routes_.find(id);
    if(it == routes_.end())
        return absl::NotFoundError("unknown story");
    if(it->second.epoch != epoch)
        return absl::FailedPreconditionError("stale epoch");
    return absl::OkStatus();
}

absl::Status ConfigMembership::registerProcess(Process)
{
    return absl::UnimplementedError("a keeper does not register processes");
}

absl::Status ConfigMembership::heartbeat(std::string, std::string, uint64_t)
{
    return absl::UnimplementedError("a keeper does not track heartbeats");
}

} // namespace chronolog::keeper
