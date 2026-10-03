#pragma once

#include <vector>
#include "chrono-player/replay/HotSource.h"

namespace chronolog::player
{

// Where the Player learns the story Route, which names every Keeper to ask.
class RouteSource
{
public:
    virtual ~RouteSource() = default;
    virtual absl::StatusOr<Route> route(StoryId story) const = 0;
    virtual absl::StatusOr<RouteState> routeState(StoryId story) const
    {
        auto r = route(story);
        if(!r.ok())
            return r.status();
        RouteState state;
        state.route = *r;
        return state;
    }
    virtual absl::StatusOr<RouteState>
    routeStateAfter(StoryId story, Epoch, std::chrono::system_clock::time_point) const
    {
        return routeState(story);
    }
    virtual bool physicalPolicy(StoryId) const { return false; }
    virtual int64_t skewLimitNs() const { return 60000000000LL; }
};

// The acquisition view. It only names laggards and never decides completeness.
class WriterSource
{
public:
    virtual ~WriterSource() = default;
    virtual std::vector<WriterAssignment> writers(StoryId story) const = 0;
};

// One fixed Route for every story, from configuration.
class StaticRouteSource final: public RouteSource
{
public:
    explicit StaticRouteSource(Route route)
        : route_(std::move(route))
    {}
    absl::StatusOr<Route> route(StoryId) const override { return route_; }

private:
    Route route_;
};

} // namespace chronolog::player
