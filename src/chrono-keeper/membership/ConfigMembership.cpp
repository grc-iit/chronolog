#include "membership/ConfigMembership.h"

#include <mutex>

namespace chronolog::keeper
{

ConfigMembership::ConfigMembership(const std::vector<StaticRoute>& seed, RouteLookup lookup)
    : lookup_(std::move(lookup))
{
    for(const auto& entry: seed) routes_[entry.story_id] = entry.route;
}

void ConfigMembership::setRoute(StoryId id, Route route)
{
    std::unique_lock lock(mutex_);
    if(routes_.contains(id) && route.epoch < routes_[id].epoch)
        return;
    routes_[id] = std::move(route);
}

void ConfigMembership::setRouteState(StoryId id, RouteState state)
{
    std::unique_lock lock(mutex_);
    if(routes_.contains(id) && state.route.epoch < routes_[id].epoch)
        return;
    routes_[id] = state.route;
    states_[id] = std::move(state);
}
absl::StatusOr<RouteState> ConfigMembership::routeState(StoryId id) const
{
    auto r = route(id);
    if(!r.ok())
        return r.status();
    std::shared_lock lock(mutex_);
    auto it = states_.find(id);
    if(it != states_.end())
        return it->second;
    RouteState s;
    s.route = *r;
    return s;
}
absl::StatusOr<Route> ConfigMembership::route(StoryId id) const
{
    {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(id);
        if(it != routes_.end())
            return it->second;
    }
    if(!lookup_)
        return absl::NotFoundError("unknown story");
    auto learned = lookup_(id);
    if(!learned.ok())
        return learned.status();
    std::unique_lock lock(mutex_);
    return routes_.try_emplace(id, std::move(*learned)).first->second;
}

absl::Status ConfigMembership::validateEpoch(StoryId id, Epoch epoch) const
{
    auto current = route(id);
    if(!current.ok())
        return current.status();
    if(current->epoch != epoch)
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
