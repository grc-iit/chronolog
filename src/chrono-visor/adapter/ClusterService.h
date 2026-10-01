#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "catalog/AcquisitionFeed.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/metadata_store.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{

// chronolog.internal.v1.Cluster: process registration, heartbeat, route
// distribution and acquisition updates. ReadClock is not overridden, so it returns
// UNIMPLEMENTED until the Clock port. Route change notification lands in PR 8, so
// WatchRoutes sends one full snapshot and then holds the stream open.
// WatchAcquisitions sends one snapshot of the acquisitions assigned to the Keeper as
// of revision R, then every change above R in revision order.
class ClusterService final: public internal::v1::Cluster::CallbackService
{
public:
    class Stream
    {
    public:
        virtual ~Stream() = default;
        virtual void shutdown() = 0;
        // Drains whatever is pending. Safe to call at any time from any thread.
        virtual void wake() = 0;
    };

    ClusterService(StaticRouteMembership& membership,
                   const MetadataStore& store,
                   const AcquisitionLedger& ledger,
                   AcquisitionFeed& feed);

    grpc::ServerUnaryReactor* Register(grpc::CallbackServerContext* context,
                                       const internal::v1::RegisterRequest* request,
                                       internal::v1::RegisterResponse* response) override;
    grpc::ServerUnaryReactor* Heartbeat(grpc::CallbackServerContext* context,
                                        const internal::v1::HeartbeatRequest* request,
                                        internal::v1::HeartbeatResponse* response) override;
    grpc::ServerWriteReactor<internal::v1::WatchRoutesResponse>*
    WatchRoutes(grpc::CallbackServerContext* context, const internal::v1::WatchRoutesRequest* request) override;
    grpc::ServerWriteReactor<internal::v1::WatchAcquisitionsResponse>*
    WatchAcquisitions(grpc::CallbackServerContext* context,
                      const internal::v1::WatchAcquisitionsRequest* request) override;

    // Ends every open stream with OK. Call before Server::Shutdown so held streams
    // do not wait for the shutdown deadline.
    void shutdown();

private:
    template <class Msg>
    grpc::ServerWriteReactor<Msg>* startStream(std::deque<Msg> initial,
                                               std::function<std::optional<Msg>()> pull,
                                               std::function<bool()> failed,
                                               std::function<void(std::function<void()> wake)> attach,
                                               std::function<void()> cleanup);
    void forget(Stream* stream);
    // Routes of every live story, or the failure that prevented listing them.
    absl::StatusOr<std::vector<internal::v1::RouteUpdate>> routeSnapshot() const;

    StaticRouteMembership& membership_;
    const MetadataStore& store_;
    const AcquisitionLedger& ledger_;
    AcquisitionFeed& feed_;
    std::mutex mutex_;
    bool closed_{};
    std::set<std::shared_ptr<Stream>> streams_;
};

} // namespace chronolog::visor
