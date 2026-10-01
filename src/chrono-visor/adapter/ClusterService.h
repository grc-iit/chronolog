#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <vector>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "catalog/AcquisitionFeed.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/metadata_store.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{
class RaftMetadataStore;
class WorkerPool;

// chronolog.internal.v1.Cluster: process registration, heartbeat, route
// distribution and acquisition updates. Dynamic replicas forward unary calls and
// publish routes from their applied Catalog state.
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
                   AcquisitionFeed& feed,
                   RaftMetadataStore* raft = nullptr,
                   WorkerPool* pool = nullptr,
                   std::chrono::milliseconds failure_timeout = std::chrono::milliseconds(15000));

    grpc::ServerUnaryReactor* ExtendCeiling(grpc::CallbackServerContext*,
                                            const internal::v1::ExtendCeilingRequest*,
                                            internal::v1::ExtendCeilingResponse*) override;
    grpc::ServerUnaryReactor* DrainKeeper(grpc::CallbackServerContext*,
                                          const internal::v1::KeeperRequest*,
                                          internal::v1::MembershipResponse*) override;
    grpc::ServerUnaryReactor* JoinKeeper(grpc::CallbackServerContext*,
                                         const internal::v1::KeeperRequest*,
                                         internal::v1::MembershipResponse*) override;
    grpc::ServerUnaryReactor* AbandonKeeper(grpc::CallbackServerContext*,
                                            const internal::v1::KeeperRequest*,
                                            internal::v1::MembershipResponse*) override;
    grpc::ServerUnaryReactor* ListMembers(grpc::CallbackServerContext*,
                                          const internal::v1::ListMembersRequest*,
                                          internal::v1::MembershipResponse*) override;
    grpc::ServerUnaryReactor* ReadClock(grpc::CallbackServerContext* context,
                                        const internal::v1::ReadClockRequest* request,
                                        internal::v1::ReadClockResponse* response) override;
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
    template <class Request, class Response>
    grpc::ServerUnaryReactor* dynamicCall(grpc::CallbackServerContext*, const Request*, Response*, int operation);
    template <class Msg>
    grpc::ServerWriteReactor<Msg>* startStream(std::deque<Msg> initial,
                                               std::function<std::optional<Msg>()> pull,
                                               std::function<bool()> failed,
                                               std::function<void(std::function<void()> wake)> attach,
                                               std::function<void()> cleanup);
    void forget(Stream* stream);
    // Routes of every live story, or the failure that prevented listing them.
    absl::StatusOr<std::vector<internal::v1::RouteUpdate>> routeSnapshot() const;

    RaftMetadataStore* raft_;
    WorkerPool* pool_;
    StaticRouteMembership& membership_;
    const MetadataStore& store_;
    const AcquisitionLedger& ledger_;
    AcquisitionFeed& feed_;
    std::mutex mutex_;
    bool closed_{};
    std::set<std::shared_ptr<Stream>> streams_;
    std::jthread route_notifications_;
    std::mutex heartbeat_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> heartbeats_;
    uint64_t leader_term_{};
    std::chrono::steady_clock::time_point leader_since_;
    std::chrono::milliseconds failure_timeout_{15000};
};

} // namespace chronolog::visor
