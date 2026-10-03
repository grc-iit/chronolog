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
    if(tombstoned_.contains(id) || (routes_.contains(id) && route.epoch < routes_[id].epoch))
        return;
    routes_[id] = std::move(route);
    ++generation_;
}

void ConfigMembership::setRouteState(StoryId id, RouteState state, uint64_t revision)
{
    std::unique_lock lock(mutex_);
    if(tombstoned_.contains(id) || revision < applied_revision_ ||
       (routes_.contains(id) && state.route.epoch < routes_[id].epoch))
        return;
    routes_[id] = state.route;
    states_[id] = std::move(state);
    applied_revision_ = std::max(applied_revision_, revision);
    ++generation_;
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
void ConfigMembership::acknowledgeRoutes(uint64_t revision)
{
    std::unique_lock lock(mutex_);
    if(revision > applied_revision_)
    {
        applied_revision_ = revision;
        ++generation_;
    }
}

void ConfigMembership::tombstone(StoryId id)
{
    std::unique_lock lock(mutex_);
    tombstoned_.insert(id);
    routes_.erase(id);
    states_.erase(id);
    ++generation_;
}

absl::Status ConfigMembership::resolve(StoryId id, std::function<void()> drop)
{
    uint64_t generation;
    {
        std::shared_lock lock(mutex_);
        if(tombstoned_.contains(id) || routes_.contains(id))
            return absl::OkStatus();
        generation = generation_;
    }
    if(!lookup_)
        return absl::NotFoundError("unknown story");
    auto learned = lookup_(id);
    if(!learned.ok())
        return learned.status();
    bool first_tombstone = false;
    {
        std::unique_lock lock(mutex_);
        // GetStory has no revision. It may fill only a missing entry in an unchanged table.
        // Tombstones precede both guards and never lower the applied revision (W10.17).
        if(learned->tombstoned)
        {
            first_tombstone = tombstoned_.insert(id).second;
            routes_.erase(id);
            states_.erase(id);
            ++generation_;
        }
        else if(!tombstoned_.contains(id) && !routes_.contains(id))
        {
            if(generation != generation_)
                return absl::UnavailableError("route read superseded by watch");
            routes_.emplace(id, std::move(learned->route));
            ++generation_;
        }
    }
    if(first_tombstone && drop)
        drop();
    return absl::OkStatus();
}

absl::StatusOr<Route> ConfigMembership::route(StoryId id) const
{
    std::shared_lock lock(mutex_);
    if(tombstoned_.contains(id))
        return absl::FailedPreconditionError("story was destroyed");
    auto it = routes_.find(id);
    if(it == routes_.end())
        return absl::NotFoundError("unknown story");
    return it->second;
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
