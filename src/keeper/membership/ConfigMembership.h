#pragma once

#include <map>
#include <shared_mutex>
#include <vector>
#include <set>

#include "KeeperConfig.h"
#include "chronolog/membership.h"

namespace chronolog::keeper
{

// Route table seeded from keeper.json and replaced story by story as the Visor streams
// routes. A Keeper is not the process registry, so registration and heartbeat are
// UNIMPLEMENTED here.
class ConfigMembership final: public Membership
{
public:
    struct RouteRead
    {
        Route route;
        bool tombstoned{};
    };
    using RouteLookup = std::function<absl::StatusOr<RouteRead>(StoryId)>;

    explicit ConfigMembership(const std::vector<StaticRoute>& seed = {}, RouteLookup lookup = nullptr);

    void setRoute(StoryId id, Route route);
    void setRouteState(StoryId id, RouteState state, uint64_t revision = 0);
    void acknowledgeRoutes(uint64_t revision);
    void tombstone(StoryId id);
    // Only resolve may block. Membership contract methods use the cache.
    absl::Status resolve(StoryId id, std::function<void()> drop = {});
    absl::StatusOr<RouteState> routeState(StoryId id) const override;

    absl::StatusOr<Route> route(StoryId id) const override;
    absl::Status validateEpoch(StoryId id, Epoch epoch) const override;
    absl::Status registerProcess(Process process) override;
    absl::Status heartbeat(std::string id, std::string instance, uint64_t applied_revision) override;

private:
    mutable std::shared_mutex mutex_;
    std::map<StoryId, Route> routes_;
    std::map<StoryId, RouteState> states_;
    std::set<StoryId> tombstoned_;
    uint64_t applied_revision_{};
    uint64_t generation_{};
    const RouteLookup lookup_;
};

} // namespace chronolog::keeper
