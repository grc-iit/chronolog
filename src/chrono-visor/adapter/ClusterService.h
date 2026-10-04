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
#include "catalog/SqliteMetadataStore.h"
#include "chronolog/clock.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/metadata_store.h"
#include "membership/StaticRouteMembership.h"

namespace chronolog::visor
{
class RaftMetadataStore;
class WorkerPool;

// What this replica stamps on the replies it generates: its physical Clock, its identity and the Keeper
// timeouts it issues in RegisterResponse.policy.
struct ClusterServiceOptions
{
    // Every physical reading comes from here (I8.3). Null reads CLOCK_REALTIME as Unsynced with no bound.
    const Clock* clock = nullptr;
    std::string replica_id;
    std::string instance;
    std::chrono::milliseconds release_fence_timeout{2000};
};

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
                   std::chrono::milliseconds failure_timeout = std::chrono::milliseconds(15000),
                   std::chrono::milliseconds route_poll_period = std::chrono::milliseconds(100),
                   ClusterServiceOptions options = {});
    ~ClusterService() override;

    grpc::ServerUnaryReactor* ExtendCeiling(grpc::CallbackServerContext*,
                                            const internal::v1::ExtendCeilingRequest*,
                                            internal::v1::ExtendCeilingResponse*) override;
    grpc::ServerUnaryReactor* DrainKeeper(grpc::CallbackServerContext*,
                                          const internal::v1::DrainKeeperRequest*,
                                          internal::v1::DrainKeeperResponse*) override;
    grpc::ServerUnaryReactor* JoinKeeper(grpc::CallbackServerContext*,
                                         const internal::v1::JoinKeeperRequest*,
                                         internal::v1::JoinKeeperResponse*) override;
    grpc::ServerUnaryReactor* AbandonKeeper(grpc::CallbackServerContext*,
                                            const internal::v1::AbandonKeeperRequest*,
                                            internal::v1::AbandonKeeperResponse*) override;
    grpc::ServerUnaryReactor* ListMembers(grpc::CallbackServerContext*,
                                          const internal::v1::ListMembersRequest*,
                                          internal::v1::ListMembersResponse*) override;
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
    grpc::ServerUnaryReactor* dynamicCall(grpc::CallbackServerContext*, const Request*, Response*);
    template <class Msg>
    grpc::ServerWriteReactor<Msg>* startStream(std::deque<Msg> initial,
                                               std::function<std::optional<Msg>()> pull,
                                               std::function<bool()> failed,
                                               std::function<void(std::function<void()> wake)> attach,
                                               std::function<void()> cleanup);
    void forget(Stream* stream);
    // Routes of every live story, or the failure that prevented listing them.
    absl::StatusOr<std::vector<internal::v1::RouteUpdate>> routeSnapshot() const;
    // This replica's physical reading, authority tick and identity. Returns false when no reading exists.
    template <class Response>
    bool stampClock(Response* response) const;
    void issueTimeouts(internal::v1::MembershipPolicy* policy) const;
    // Caller holds heartbeat_mutex_. Liveness stamps and parked applied revisions belong to the leader term that
    // recorded them. Every reader and writer enters the current term first, so the previous term's entries are dropped
    // before anything of this term is recorded and an entry of the current term is never discarded.
    void enterTerm(uint64_t term);

    RaftMetadataStore* raft_;
    WorkerPool* pool_;
    StaticRouteMembership& membership_;
    const MetadataStore& store_;
    const AcquisitionLedger& ledger_;
    AcquisitionFeed& feed_;
    std::mutex mutex_;
    bool closed_{};
    std::set<std::shared_ptr<Stream>> streams_;
    std::mutex heartbeat_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> heartbeats_;
    std::map<std::string, internal::v1::AppliedRouteRevision> applied_routes_;
    // The term heartbeats_ and applied_routes_ belong to.
    uint64_t recorded_term_{};
    // The term whose lease the failure detector last saw begin, at leader_since_.
    uint64_t leader_term_{};
    std::chrono::steady_clock::time_point leader_since_;
    // The instance and time of each process's latest Heartbeat as it reached this replica, before the call queues or
    // is refused, so the leader's own queueing, slow apply or lease lapse never reads as Keeper silence (I4.9). Never
    // held across I/O.
    std::mutex arrival_mutex_;
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> arrivals_;
    std::chrono::milliseconds failure_timeout_{15000};
    std::unique_ptr<Clock> owned_clock_;
    const Clock* clock_;
    internal::v1::ClockResponder responder_;
    std::chrono::milliseconds release_fence_timeout_;
    std::shared_ptr<SqliteMetadataStore::RouteSignal> route_signal_;
    std::jthread route_notifications_;
};

} // namespace chronolog::visor
