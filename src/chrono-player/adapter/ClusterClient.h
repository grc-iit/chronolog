#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "chrono-player/replay/RouteSource.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::player
{

// Registers the Player with the Visor and serves story Routes from the snapshot Register
// returns. The Visor has no per-story route lookup, so a miss re-registers (idempotent for
// one instance) to pick up stories created since the last snapshot.
class ClusterClient final: public RouteSource
{
public:
    ClusterClient(std::shared_ptr<grpc::Channel> visor_internal,
                  Process self,
                  std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

    // Called for every Route learned, so the writer directory can follow its Keepers.
    void onRoute(std::function<void(const Route&)> callback);

    absl::Status registerSelf();
    absl::StatusOr<Route> route(StoryId story) const override;
    absl::StatusOr<RouteState> routeState(StoryId story) const override;
    bool physicalPolicy(StoryId story) const override;
    int64_t skewLimitNs() const override;

private:
    absl::Status refresh() const;

    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    Process self_;
    std::chrono::milliseconds deadline_;
    mutable std::mutex mu_;
    mutable std::map<StoryId, RouteState> routes_;
    mutable std::map<StoryId, bool> physical_policy_;
    mutable int64_t skew_limit_ns_{60000000000LL};
    mutable bool dynamic_{};
    mutable uint64_t revision_{};
    mutable std::map<StoryId, uint64_t> route_revisions_;
    std::function<void(const Route&)> on_route_;
};

} // namespace chronolog::player
