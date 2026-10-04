#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include "rpc/VisorClockAudit.h"
#include "chrono-player/replay/RouteSource.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::player
{

// Registers once and follows WatchRoutes. Cached routes remain usable across a reconnect;
// FetchHot's expected epoch prevents a stale route from claiming completeness (I6.11).
class ClusterClient final: public RouteSource
{
public:
    using TombstoneLookup = std::function<absl::StatusOr<bool>(StoryId)>;
    ClusterClient(std::shared_ptr<grpc::Channel> visor_internal,
                  Process self,
                  std::chrono::milliseconds deadline = std::chrono::milliseconds(2000),
                  TombstoneLookup lookup = {},
                  std::shared_ptr<VisorClockAudit> clock_audit = {});
    ~ClusterClient() override;

    // Called for every Route learned, so the writer directory can follow its Keepers.
    void onRoute(std::function<void(const Route&)> callback);

    absl::Status registerSelf();
    absl::StatusOr<Route> route(StoryId story) const override;
    absl::StatusOr<RouteState> routeState(StoryId story) const override;
    absl::StatusOr<RouteState>
    routeStateAfter(StoryId story, Epoch epoch, std::chrono::system_clock::time_point deadline) const override;
    bool physicalPolicy(StoryId story) const override;
    int64_t skewLimitNs() const override;

private:
    absl::Status refresh(std::stop_token stop = {}) const;
    void apply(const internal::v1::RouteUpdate& update, bool snapshot = false) const;
    void watch(std::stop_token stop) const;
    void monitor(std::stop_token stop) const;

    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    Process self_;
    std::chrono::milliseconds deadline_;
    TombstoneLookup lookup_;
    // Records on whichever thread runs Register or Heartbeat; logs only from monitor().
    std::shared_ptr<VisorClockAudit> clock_audit_;
    mutable std::mutex registration_mu_;
    mutable bool registered_{};
    mutable std::mutex mu_;
    mutable std::condition_variable_any cv_;
    mutable std::map<StoryId, RouteState> routes_;
    mutable std::map<StoryId, bool> physical_policy_;
    mutable int64_t skew_limit_ns_{60000000000LL};
    mutable uint64_t revision_{};
    mutable std::map<StoryId, uint64_t> route_revisions_, learned_revisions_;
    mutable std::set<StoryId> tombstoned_, looking_up_;
    mutable std::map<StoryId, absl::Status> looked_up_;
    mutable std::map<StoryId, unsigned> reconcile_;
    std::function<void(const Route&)> on_route_;
    mutable std::jthread watcher_, monitor_;
};

} // namespace chronolog::player
