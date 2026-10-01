#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chronolog/types.h"

namespace chronolog::visor
{

// The static cluster shape from configuration. The Catalog stores use it to build
// the Route returned by acquire, and StaticRouteMembership serves it from route().
struct Topology
{
    std::vector<KeeperRef> keepers;
    std::string grapher;
    std::string player;

    Route routeFor(Epoch epoch) const { return Route{epoch, keepers, grapher, player}; }

    // The single Keeper for a writer. The mapping depends only on writer_id and the
    // keeper list, so it is stable for every acquire within one epoch (I7.5).
    absl::StatusOr<KeeperRef> assignKeeper(uint64_t writer_id, Epoch) const
    {
        if(keepers.empty())
            return absl::FailedPreconditionError("topology has no keepers");
        return keepers[writer_id % keepers.size()];
    }
};

} // namespace chronolog::visor
