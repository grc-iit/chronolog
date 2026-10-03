#include "adapter/ClusterService.h"
#include "dynamic/MembershipState.h"

#include <chrono>
#include <algorithm>
#include <deque>
#include <optional>

#include <absl/log/log.h>
#include "adapter/Convert.h"
#include "rpc/Channel.h"
#include "adapter/WorkerPool.h"
#include "clock/SystemClock.h"
#include "raft/RaftMetadataStore.h"

namespace chronolog::visor
{

namespace
{

// Server stream that sends `initial`, then whatever `pull` yields on each wake(),
// and holds open until shutdown or cancellation. The Subscription behind `pull` and
// the service registry own it through shared_ptr, so a late wake() is harmless.
template <class Msg>
class WriteStream final
    : public grpc::ServerWriteReactor<Msg>
    , public ClusterService::Stream
    , public std::enable_shared_from_this<WriteStream<Msg>>
{
public:
    WriteStream(std::deque<Msg> initial,
                std::function<std::optional<Msg>()> pull,
                std::function<bool()> failed,
                std::function<void(ClusterService::Stream*)> on_done)
        : pending_(std::move(initial))
        , pull_(std::move(pull))
        , failed_(std::move(failed))
        , on_done_(std::move(on_done))
    {}

    void wake() override
    {
        std::lock_guard lock(mutex_);
        next();
    }

    void shutdown() override
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        next();
    }

    void OnWriteDone(bool ok) override
    {
        std::lock_guard lock(mutex_);
        writing_ = false;
        if(!ok)
        {
            stop_ = true;
            status_ = grpc::Status(grpc::StatusCode::UNAVAILABLE, "stream write failed");
        }
        next();
    }

    void OnCancel() override
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        status_ = grpc::Status::CANCELLED;
        next();
    }

    void OnDone() override
    {
        // on_done_ releases the registry's reference and may destroy this object, so
        // run a local copy and touch no member afterwards.
        auto done = std::move(on_done_);
        done(this);
    }

private:
    // Caller holds mutex_.
    void next()
    {
        if(finished_ || writing_)
            return;
        if(stop_)
        {
            finished_ = true;
            this->Finish(status_);
            return;
        }
        if(failed_ && failed_())
        {
            finished_ = true;
            this->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "subscriber fell behind, resubscribe"));
            return;
        }
        if(!pending_.empty())
        {
            current_ = std::move(pending_.front());
            pending_.pop_front();
        }
        else if(auto message = pull_ ? pull_() : std::nullopt)
        {
            current_ = std::move(*message);
        }
        else
        {
            if(failed_ && failed_())
            {
                finished_ = true;
                this->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "subscriber fell behind, resubscribe"));
            }
            return;
        }
        writing_ = true;
        this->StartWrite(&current_);
    }

    // A message can arrive from the store while OnWriteDone runs on another thread.
    std::recursive_mutex mutex_;
    std::deque<Msg> pending_;
    std::function<std::optional<Msg>()> pull_;
    std::function<bool()> failed_;
    std::function<void(ClusterService::Stream*)> on_done_;
    Msg current_;
    bool writing_{};
    bool stop_{};
    bool finished_{};
    grpc::Status status_{grpc::Status::OK};
};

// A stream that fails before it starts, used when the service is already closed.
template <class Msg>
class FailedStream final: public grpc::ServerWriteReactor<Msg>
{
public:
    explicit FailedStream(grpc::Status status) { this->Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

// Only a Synced reading with a finite bound goes out as Synced; a bound is never manufactured (I8.3).
v1::TimeReading wireReading(const absl::StatusOr<TimeReading>& local)
{
    v1::TimeReading reading;
    if(!local.ok() || local->status == ClockStatus::Unavailable)
    {
        reading.set_status(v1::CLOCK_STATUS_UNAVAILABLE);
        return reading;
    }
    reading.set_physical_ns(local->physical_ns);
    if(local->status == ClockStatus::Synced && local->uncertainty_ns)
    {
        reading.set_status(v1::CLOCK_STATUS_SYNCED);
        reading.set_uncertainty_ns(*local->uncertainty_ns);
    }
    else
        reading.set_status(v1::CLOCK_STATUS_UNSYNCED);
    return reading;
}

uint64_t authorityTickNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

} // namespace

ClusterService::ClusterService(StaticRouteMembership& membership,
                               const MetadataStore& store,
                               const AcquisitionLedger& ledger,
                               AcquisitionFeed& feed,
                               RaftMetadataStore* raft,
                               WorkerPool* pool,
                               std::chrono::milliseconds failure_timeout,
                               std::chrono::milliseconds route_poll_period,
                               ClusterServiceOptions options)
    : raft_(raft)
    , pool_(pool)
    , membership_(membership)
    , store_(store)
    , ledger_(ledger)
    , feed_(feed)
    , failure_timeout_(failure_timeout)
    , owned_clock_(options.clock ? nullptr : std::make_unique<SystemClock>())
    , clock_(options.clock ? options.clock : owned_clock_.get())
    , release_fence_timeout_(options.release_fence_timeout)
{
    responder_.set_replica_id(std::move(options.replica_id));
    responder_.set_instance(std::move(options.instance));
    const auto* history = raft_ ? &raft_->appliedStore() : dynamic_cast<const SqliteMetadataStore*>(&store_);
    if(history)
    {
        route_signal_ = history->watchRouteChanges();
        route_notifications_ = std::jthread(
                [this, route_poll_period](std::stop_token stop)
                {
                    auto next_tick = std::chrono::steady_clock::now() + route_poll_period;
                    uint64_t generation = raft_ ? raft_->appliedStore().snapshotGeneration() : 0;
                    while(!stop.stop_requested())
                    {
                        (void)route_signal_->try_acquire_until(next_tick);
                        while(route_signal_->try_acquire()) {}
                        if(stop.stop_requested())
                            return;
                        const bool tick = std::chrono::steady_clock::now() >= next_tick;
                        if(tick)
                            next_tick = std::chrono::steady_clock::now() + route_poll_period;
                        std::set<std::shared_ptr<Stream>> streams;
                        {
                            std::lock_guard lock(mutex_);
                            if(closed_)
                                return;
                            streams = streams_;
                        }
                        if(tick && raft_ && raft_->leaderLease())
                        {
                            std::vector<std::string> failed;
                            {
                                std::lock_guard heartbeat_lock(heartbeat_mutex_);
                                auto now = std::chrono::steady_clock::now();
                                // A term has one leader, so a lease lapse inside it keeps this term's liveness.
                                if(leader_term_ != raft_->term())
                                {
                                    LOG(INFO) << "visor_leader term=" << raft_->term();
                                    leader_term_ = raft_->term();
                                    leader_since_ = now;
                                    heartbeats_.clear();
                                    applied_routes_.clear();
                                }
                                if(now - leader_since_ >= failure_timeout_)
                                {
                                    auto state = raft_->appliedStore().membershipLivenessState();
                                    if(!state.ok())
                                        continue;
                                    for(const auto& member: state->members())
                                        if(member.joined() && !member.process().instance().empty() &&
                                           !raft_->appliedStore().membershipWouldEmpty(member.process().process_id()))
                                        {
                                            auto it = heartbeats_.find(member.process().process_id());
                                            if(it == heartbeats_.end() || now - it->second >= failure_timeout_)
                                                failed.push_back(member.process().process_id());
                                        }
                                }
                            }
                            for(const auto& id: failed)
                            {
                                LOG(WARNING) << "keeper_silent process=" << id << ", proposing drain";
                                internal::v1::CatalogCommand command;
                                command.mutable_membership()->mutable_drain()->set_process_id(id);
                                (void)raft_->propose(command);
                            }
                        }
                        auto current = raft_ ? raft_->appliedStore().snapshotGeneration() : 0;
                        for(auto& stream: streams)
                        {
                            if(current != generation)
                                stream->shutdown();
                            else
                                stream->wake();
                        }
                        generation = current;
                    }
                });
    }
}

ClusterService::~ClusterService() { shutdown(); }

grpc::ServerUnaryReactor* ClusterService::ReadClock(grpc::CallbackServerContext* context,
                                                    const internal::v1::ReadClockRequest* request,
                                                    internal::v1::ReadClockResponse* response)
{
    auto* reactor = context->DefaultReactor();
    // A clock observation is not a Catalog read: a static replica and an audited replica answer for themselves.
    if(raft_ && !request->audit_local_replica() && !raft_->leaderLease())
    {
        auto task = [this, context, request, response, reactor]
        {
            auto endpoint = raft_->leaderEndpoint(true);
            if(endpoint.empty() || raft_->isLocalLeader())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease"));
                return;
            }
            grpc::ClientContext ctx;
            rpc::forwardingContext(ctx, context->deadline());
            auto stub = internal::v1::Cluster::NewStub(rpc::peerChannel(endpoint));
            reactor->Finish(stub->ReadClock(&ctx, *request, response));
        };
        if(!pool_ || !SubmitCall(*pool_, context, reactor, std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
        return reactor;
    }
    if(!stampClock(response))
    {
        response->Clear();
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no physical clock reading"));
        return reactor;
    }
    reactor->Finish(grpc::Status::OK);
    return reactor;
}

template <class Response>
bool ClusterService::stampClock(Response* response) const
{
    *response->mutable_physical() = wireReading(clock_->now());
    response->set_authority_tick_ns(authorityTickNs());
    *response->mutable_clock_responder() = responder_;
    return response->physical().status() != v1::CLOCK_STATUS_UNAVAILABLE;
}

void ClusterService::issueTimeouts(internal::v1::MembershipPolicy* policy) const
{
    policy->set_keeper_failure_timeout_ms(static_cast<uint32_t>(failure_timeout_.count()));
    policy->set_release_fence_timeout_ms(static_cast<uint32_t>(release_fence_timeout_.count()));
}

grpc::ServerUnaryReactor* ClusterService::Register(grpc::CallbackServerContext* context,
                                                   const internal::v1::RegisterRequest* request,
                                                   internal::v1::RegisterResponse* response)
{
    if(raft_)
        return dynamicCall(context, request, response);
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    auto task = [this, request = *request, response, reactor]
    {
        auto process = convert::fromProto(request.process());
        absl::Status status = process.ok() ? absl::OkStatus() : process.status();
        const auto* sqlite = dynamic_cast<const SqliteMetadataStore*>(&store_);
        auto constants = sqlite ? sqlite->physicalPolicy() : absl::StatusOr<PhysicalPolicy>(PhysicalPolicy{});
        if(!constants.ok())
            status = constants.status();
        if(status.ok() && request.policy_version() != 0 && request.policy_version() != constants->version)
            status = absl::FailedPreconditionError("policy version mismatch");
        if(status.ok())
        {
            if(auto* writable = const_cast<SqliteMetadataStore*>(sqlite);
               writable && process->role == ProcessRole::Keeper)
                status = writable->registerStaticPolicy(process->id, request.policy_version());
            if(status.ok())
                status = membership_.registerProcess(*process);
        }
        *response->mutable_status() = convert::toProto(status);
        auto* policy = response->mutable_policy();
        if(constants.ok())
        {
            policy->set_version(constants->version);
            policy->set_acceptance_window_ns(constants->acceptance_window_ns);
            policy->set_skew_limit_ns(constants->skew_limit_ns);
            policy->set_hlc_lead_ns(constants->hlc_lead_ns);
            policy->set_uncertainty_cap_ns(constants->uncertainty_cap_ns);
        }
        issueTimeouts(policy);
        if(status.ok())
        {
            auto routes = routeSnapshot();
            if(routes.ok())
                for(auto& route: *routes) *response->add_routes() = std::move(route);
        }
        (void)stampClock(response);
        reactor->Finish(grpc::Status::OK);
    };
    if(pool_)
    {
        if(!SubmitCall(*pool_, context, reactor, std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
    }
    else
        task();
    return reactor;
}

namespace
{
// Bounded tuple list from HeartbeatRequest field 8; entries past the bound supply no evidence.
std::vector<RenewAcquisition> evidenceTuples(const internal::v1::HeartbeatRequest& request)
{
    std::vector<RenewAcquisition> tuples;
    const int count = std::min(request.admission_evidence_size(), 65536);
    tuples.reserve(count);
    for(int i = 0; i < count; ++i)
    {
        const auto& e = request.admission_evidence(i);
        tuples.push_back({e.story_id(), e.writer_id(), e.incarnation()});
    }
    return tuples;
}
} // namespace

grpc::ServerUnaryReactor* ClusterService::Heartbeat(grpc::CallbackServerContext* context,
                                                    const internal::v1::HeartbeatRequest* request,
                                                    internal::v1::HeartbeatResponse* response)
{
    if(raft_)
        return dynamicCall(context, request, response);
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    auto task = [this, request = *request, response, reactor]
    {
        absl::Status status =
                membership_.heartbeat(request.process_id(), request.instance(), request.applied_revision());
        // Evidence renews only after instance validation and never changes the heartbeat result.
        if(status.ok() && request.admission_evidence_size())
            if(auto* sqlite = const_cast<SqliteMetadataStore*>(dynamic_cast<const SqliteMetadataStore*>(&store_)))
                (void)sqlite->acceptKeeperEvidence(request.process_id(), evidenceTuples(request));
        if(status.ok() && !request.stories_without_physical_policy().empty())
        {
            auto process = membership_.process(request.process_id());
            if(!process || process->role != ProcessRole::Grapher)
                status = absl::InvalidArgumentError("physical policy reports require a Grapher");
            else if(request.stories_without_physical_policy_size() > 65536 ||
                    std::find(request.stories_without_physical_policy().begin(),
                              request.stories_without_physical_policy().end(),
                              0) != request.stories_without_physical_policy().end())
                status = absl::InvalidArgumentError("invalid physical policy story list");
            else if(auto* sqlite = const_cast<SqliteMetadataStore*>(dynamic_cast<const SqliteMetadataStore*>(&store_)))
                status = sqlite->clearPhysicalPolicy({request.stories_without_physical_policy().begin(),
                                                      request.stories_without_physical_policy().end()});
            else
                status = absl::UnimplementedError("physical policy clearing needs a persisted Catalog");
        }
        *response->mutable_status() = convert::toProto(status);
        (void)stampClock(response);
        reactor->Finish(grpc::Status::OK);
    };
    if((request->stories_without_physical_policy().empty() && request->admission_evidence().empty()) || !pool_)
        task();
    else if(!SubmitCall(*pool_, context, reactor, std::move(task)))
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
    return reactor;
}

template <class Msg>
grpc::ServerWriteReactor<Msg>* ClusterService::startStream(std::deque<Msg> initial,
                                                           std::function<std::optional<Msg>()> pull,
                                                           std::function<bool()> failed,
                                                           std::function<void(std::function<void()> wake)> attach,
                                                           std::function<void()> cleanup)
{
    std::lock_guard lock(mutex_);
    if(closed_)
        return new FailedStream<Msg>(grpc::Status(grpc::StatusCode::UNAVAILABLE, "visor is shutting down"));
    auto stream = std::make_shared<WriteStream<Msg>>(std::move(initial),
                                                     std::move(pull),
                                                     std::move(failed),
                                                     [this, cleanup = std::move(cleanup)](Stream* done)
                                                     {
                                                         if(cleanup)
                                                             cleanup();
                                                         forget(done);
                                                     });
    streams_.insert(stream);
    if(attach)
        attach(
                [weak = std::weak_ptr<Stream>(stream)]()
                {
                    if(auto strong = weak.lock())
                        strong->wake();
                });
    stream->wake();
    return stream.get();
}

void ClusterService::forget(Stream* stream)
{
    std::shared_ptr<Stream> released;
    {
        std::lock_guard lock(mutex_);
        for(auto it = streams_.begin(); it != streams_.end(); ++it)
        {
            if(it->get() == stream)
            {
                released = *it;
                streams_.erase(it);
                break;
            }
        }
    }
    // `released` drops the last reference here, outside the registry lock.
}

absl::StatusOr<std::vector<internal::v1::RouteUpdate>> ClusterService::routeSnapshot() const
{
    if(raft_)
    {
        auto state = dynamic::snapshot(raft_->appliedStore());
        return std::vector<internal::v1::RouteUpdate>(state.routes().begin(), state.routes().end());
    }
    std::vector<internal::v1::RouteUpdate> out;
    auto chronicles = store_.listChronicles();
    if(!chronicles.ok())
        return chronicles.status();
    for(const auto& chronicle: *chronicles)
    {
        if(chronicle.tombstoned)
            continue;
        auto stories = store_.listStories(chronicle.name);
        if(!stories.ok())
            return stories.status();
        for(const auto& story: *stories)
        {
            if(story.tombstoned)
                continue;
            auto route = membership_.route(story.id);
            if(!route.ok())
                continue;
            internal::v1::RouteUpdate update;
            update.set_story_id(story.id);
            if(raft_)
            {
                route->epoch = story.epoch;
                auto snapshot = ledger_.snapshotAcquisitions();
                if(!snapshot.ok())
                    return snapshot.status();
                update.set_revision(snapshot->revision);
            }
            *update.mutable_route() = convert::toProto(*route);
            if(const auto* sqlite = dynamic_cast<const SqliteMetadataStore*>(&store_))
            {
                auto persisted = sqlite->membershipRouteUpdate(story.id);
                if(!persisted.ok())
                    return persisted.status();
                update.set_revision(persisted->revision());
                update.set_physical_policy(persisted->physical_policy());
            }
            out.push_back(std::move(update));
        }
    }
    return out;
}

grpc::ServerWriteReactor<internal::v1::WatchRoutesResponse>*
ClusterService::WatchRoutes(grpc::CallbackServerContext*, const internal::v1::WatchRoutesRequest*)
{
    const SqliteMetadataStore* history =
            raft_ ? &raft_->appliedStore() : dynamic_cast<const SqliteMetadataStore*>(&store_);
    uint64_t cursor = 0;
    absl::StatusOr<std::vector<internal::v1::RouteUpdate>> routes;
    if(history)
    {
        // Snapshot and cursor share the store lock, so every later creation, policy change or destroy is a delta.
        auto state = history->membershipState();
        if(!state.ok())
            return new FailedStream<internal::v1::WatchRoutesResponse>(
                    grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(state.status().message())));
        cursor = state->revision();
        routes = std::vector<internal::v1::RouteUpdate>(state->routes().begin(), state->routes().end());
        for(auto& update: *routes) update.set_revision(cursor);
    }
    else
        routes = routeSnapshot();
    if(!routes.ok())
        return new FailedStream<internal::v1::WatchRoutesResponse>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(routes.status().message())));
    std::deque<internal::v1::WatchRoutesResponse> snapshot;
    for(auto& update: *routes)
    {
        internal::v1::WatchRoutesResponse message;
        message.set_story_id(update.story_id());
        message.set_revision(update.revision());
        message.ParseFromString(update.SerializeAsString());
        snapshot.push_back(std::move(message));
    }
    std::function<std::optional<internal::v1::WatchRoutesResponse>()> pull;
    std::function<bool()> failed;
    if(history)
    {
        auto state = std::make_shared<std::pair<uint64_t, std::deque<internal::v1::WatchRoutesResponse>>>();
        auto trimmed = std::make_shared<bool>(false);
        state->first = cursor;
        for(const auto& message: snapshot) state->first = std::max(state->first, message.revision());
        failed = [trimmed] { return *trimmed; };
        pull = [history, state, trimmed]() -> std::optional<internal::v1::WatchRoutesResponse>
        {
            if(state->second.empty())
            {
                auto revision = history->membershipRevision();
                if(!revision.ok() || *revision <= state->first)
                    return std::nullopt;
                auto loaded = history->membershipRouteChanges(state->first);
                if(!loaded.ok())
                    return std::nullopt;
                const auto& current = *loaded;
                if(state->first < current.route_history_floor())
                {
                    // Reconnect resets the subscriber's seen set for W10.17 tombstone reconciliation.
                    *trimmed = true;
                    return std::nullopt;
                }
                for(const auto& update: current.route_history())
                    if(update.revision() > state->first)
                    {
                        auto& message = state->second.emplace_back();
                        message.ParseFromString(update.SerializeAsString());
                    }
                state->first = current.revision();
            }
            if(state->second.empty())
                return std::nullopt;
            auto message = std::move(state->second.front());
            state->second.pop_front();
            return message;
        };
    }
    return startStream<internal::v1::WatchRoutesResponse>(std::move(snapshot),
                                                          std::move(pull),
                                                          std::move(failed),
                                                          nullptr,
                                                          nullptr);
}

grpc::ServerWriteReactor<internal::v1::WatchAcquisitionsResponse>*
ClusterService::WatchAcquisitions(grpc::CallbackServerContext*, const internal::v1::WatchAcquisitionsRequest* request)
{
    if(request->keeper_id().empty())
        return new FailedStream<internal::v1::WatchAcquisitionsResponse>(
                grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "keeper_id is required"));
    if(raft_ && !raft_->appliedStateCurrent())
    {
        LOG_EVERY_N_SEC(INFO, 2) << "acquisition_watch refused keeper=" << request->keeper_id()
                                 << " leader=" << raft_->leaderId() << " term=" << raft_->term();
        return new FailedStream<internal::v1::WatchAcquisitionsResponse>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, "replica has not applied the committed log of the term"));
    }
    // The subscription registers for changes before it reads the ledger, so the
    // snapshot at revision R and the queued changes above R leave no gap.
    auto subscription = feed_.subscribe(ledger_, request->keeper_id());
    if(!subscription.ok())
        return new FailedStream<internal::v1::WatchAcquisitionsResponse>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(subscription.status().message())));
    std::shared_ptr<AcquisitionFeed::Subscription> sub = *subscription;
    if(raft_)
        LOG(INFO) << "acquisition_watch keeper=" << request->keeper_id() << " revision=" << sub->snapshot().revision
                  << " leader=" << raft_->leaderId() << " term=" << raft_->term();
    std::deque<internal::v1::WatchAcquisitionsResponse> initial;
    *initial.emplace_back().mutable_snapshot() = convert::toProto(sub->snapshot());
    return startStream<internal::v1::WatchAcquisitionsResponse>(
            std::move(initial),
            [sub]() -> std::optional<internal::v1::WatchAcquisitionsResponse>
            {
                auto change = sub->pop();
                if(!change)
                    return std::nullopt;
                internal::v1::WatchAcquisitionsResponse message;
                *message.mutable_update() = convert::toProto(*change);
                return message;
            },
            [sub]() { return sub->overflowed(); },
            [sub](std::function<void()> wake) { sub->setWakeup(std::move(wake)); },
            [sub]() { sub->setWakeup(nullptr); });
}

void ClusterService::shutdown()
{
    std::set<std::shared_ptr<Stream>> open;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        open = streams_;
    }
    route_notifications_.request_stop();
    if(route_signal_)
        route_signal_->release();
    for(auto& stream: open) stream->shutdown();
}


namespace
{
template <typename Request>
constexpr bool kKeeperCall = std::is_same_v<Request, internal::v1::DrainKeeperRequest> ||
                             std::is_same_v<Request, internal::v1::JoinKeeperRequest> ||
                             std::is_same_v<Request, internal::v1::AbandonKeeperRequest>;
} // namespace
template <class Request, class Response>
grpc::ServerUnaryReactor*
ClusterService::dynamicCall(grpc::CallbackServerContext* context, const Request* request, Response* response)
{
    auto* reactor = context->DefaultReactor();
    if(!raft_)
    {
        reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "dynamic membership required"));
        return reactor;
    }
    auto task = [this, context, request, response, reactor]
    {
        if(!raft_->leaderLease())
        {
            auto endpoint = raft_->leaderEndpoint(true);
            if(endpoint.empty() || raft_->isLocalLeader())
            {
                LOG_EVERY_N_SEC(WARNING, 2)
                        << "cluster call without a leader to forward to, leader id " << raft_->leaderId();
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease"));
                return;
            }
            grpc::ClientContext ctx;
            rpc::forwardingContext(ctx, context->deadline());
            auto stub = internal::v1::Cluster::NewStub(rpc::peerChannel(endpoint));
            grpc::Status result;
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                result = stub->Register(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
                result = stub->Heartbeat(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::ExtendCeilingRequest>)
                result = stub->ExtendCeiling(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::ListMembersRequest>)
                result = stub->ListMembers(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::DrainKeeperRequest>)
                result = stub->DrainKeeper(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::JoinKeeperRequest>)
                result = stub->JoinKeeper(&ctx, *request, response);
            else
                result = stub->AbandonKeeper(&ctx, *request, response);
            if(!result.ok())
                LOG_EVERY_N_SEC(WARNING, 2) << "cluster_forward to leader " << endpoint
                                            << " failed: " << result.error_code() << " " << result.error_message();
            reactor->Finish(result);
            return;
        }
        if constexpr(std::is_same_v<Request, internal::v1::ListMembersRequest>)
        {
            auto state = dynamic::snapshot(raft_->appliedStore());
            *response->mutable_status() = convert::toProto(absl::OkStatus());
            for(const auto& m: state.members()) *response->add_members() = m;
            for(const auto& r: state.routes()) *response->add_routes() = r;
            if(!raft_->leaderLease())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "leader lease expired"));
                return;
            }
        }
        else
        {
            internal::v1::CatalogCommand command;
            auto* q = command.mutable_membership();
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                *q->mutable_register_() = *request;
            else if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
            {
                *q->mutable_heartbeat() = *request;
                // Admission evidence is leader-local lease memory and never enters a Raft entry.
                q->mutable_heartbeat()->clear_admission_evidence();
            }
            else if constexpr(std::is_same_v<Request, internal::v1::ExtendCeilingRequest>)
                *q->mutable_extend() = *request;
            else if constexpr(std::is_same_v<Request, internal::v1::DrainKeeperRequest>)
                q->mutable_drain()->set_process_id(request->process_id());
            else if constexpr(std::is_same_v<Request, internal::v1::JoinKeeperRequest>)
                q->mutable_join()->set_process_id(request->process_id());
            else
                q->mutable_abandon()->set_process_id(request->process_id());
            absl::StatusOr<std::string> result = absl::UnavailableError("not proposed");
            bool propose = true;
            if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
            {
                auto loaded = raft_->appliedStore().membershipCommandState(*q);
                if(!loaded.ok())
                {
                    reactor->Finish(
                            grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(loaded.status().message())));
                    return;
                }
                const auto& state = *loaded;
                bool current = false;
                for(const auto& m: state.members())
                    if(m.process().process_id() == request->process_id() &&
                       m.process().instance() == request->instance())
                        current = true;
                // Consumed before the evidence-only early return, so it costs no proposal.
                if(current && request->admission_evidence_size())
                    (void)raft_->acceptKeeperEvidence(request->process_id(), evidenceTuples(*request));
                if(!dynamic::heartbeatChanges(state, q->heartbeat()))
                {
                    internal::v1::HeartbeatResponse plain;
                    auto status = current ? absl::OkStatus()
                                          : absl::FailedPreconditionError("obsolete or unknown process instance");
                    *plain.mutable_status() = convert::toProto(status);
                    result = plain.SerializeAsString();
                    propose = false;
                }
            }
            {
                std::lock_guard lock(heartbeat_mutex_);
                if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
                {
                    auto loaded = raft_->appliedStore().membershipCommandState(*q);
                    if(loaded.ok())
                        for(const auto& m: loaded->members())
                            if(m.process().process_id() == request->process_id() &&
                               m.process().instance() == request->instance() &&
                               request->applied_route_revision() > m.applied_route_revision())
                            {
                                auto& applied = applied_routes_[request->process_id()];
                                if(applied.instance() != request->instance())
                                    applied.Clear();
                                applied.set_process_id(request->process_id());
                                applied.set_instance(request->instance());
                                applied.set_revision(std::max(applied.revision(), request->applied_route_revision()));
                            }
                }
                for(const auto& [id, applied]: applied_routes_) *q->add_applied_routes() = applied;
                // Liveness is refreshed before the command can apply, so no detection tick sees a newly
                // registered or joined Keeper with a stale timestamp and drains it.
                if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                    heartbeats_[request->process().process_id()] = std::chrono::steady_clock::now();
                if constexpr(std::is_same_v<Request, internal::v1::JoinKeeperRequest>)
                    heartbeats_[request->process_id()] = std::chrono::steady_clock::now();
            }
            if(propose)
                result = raft_->propose(command);
            if(!result.ok())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(result.status().message())));
                return;
            }
            if(!response->ParseFromString(*result))
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "invalid membership response"));
                return;
            }
            if(response->status().code() == 0)
            {
                if(propose)
                {
                    std::lock_guard lock(heartbeat_mutex_);
                    for(const auto& sent: q->applied_routes())
                    {
                        auto pending = applied_routes_.find(sent.process_id());
                        if(pending != applied_routes_.end() && pending->second.instance() == sent.instance() &&
                           pending->second.revision() <= sent.revision())
                            applied_routes_.erase(pending);
                    }
                }
                if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest> || kKeeperCall<Request>)
                {
                    auto current = dynamic::snapshot(raft_->appliedStore());
                    response->clear_routes();
                    for(const auto& route: current.routes()) *response->add_routes() = route;
                    if constexpr(kKeeperCall<Request>)
                    {
                        response->clear_members();
                        for(const auto& member: current.members()) *response->add_members() = member;
                    }
                }
                if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                {
                    for(const auto& endpoint: raft_->replicaEndpoints()) response->add_visor_replicas(endpoint);
                    (void)membership_.registerProcess(*convert::fromProto(request->process()));
                    std::lock_guard lock(heartbeat_mutex_);
                    heartbeats_[request->process().process_id()] = std::chrono::steady_clock::now();
                }
                if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
                {
                    auto current = raft_->appliedStore().membershipCommandState(*q);
                    bool current_instance = false;
                    if(current.ok())
                        for(const auto& member: current->members())
                            if(member.process().process_id() == request->process_id() &&
                               member.process().instance() == request->instance())
                            {
                                current_instance = true;
                                auto local = membership_.process(request->process_id());
                                if(!local || local->instance != request->instance())
                                    (void)membership_.registerProcess(*convert::fromProto(member.process()));
                            }
                    (void)membership_.heartbeat(request->process_id(),
                                                request->instance(),
                                                request->applied_revision());
                    if(current_instance)
                    {
                        std::lock_guard lock(heartbeat_mutex_);
                        auto [entry, fresh] = heartbeats_.try_emplace(request->process_id());
                        entry->second = std::chrono::steady_clock::now();
                        // The first heartbeat of a leader term, so a drain can be read against the election.
                        if(fresh)
                            LOG(INFO) << "keeper_live process=" << request->process_id() << " term=" << leader_term_;
                    }
                }
            }
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest> ||
                         std::is_same_v<Request, internal::v1::HeartbeatRequest>)
                (void)stampClock(response);
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                issueTimeouts(response->mutable_policy());
        }
        reactor->Finish(grpc::Status::OK);
    };
    if(!pool_ || !SubmitCall(*pool_, context, reactor, std::move(task)))
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
    return reactor;
}
grpc::ServerUnaryReactor* ClusterService::ExtendCeiling(grpc::CallbackServerContext* c,
                                                        const internal::v1::ExtendCeilingRequest* q,
                                                        internal::v1::ExtendCeilingResponse* r)
{
    return dynamicCall(c, q, r);
}
grpc::ServerUnaryReactor* ClusterService::DrainKeeper(grpc::CallbackServerContext* c,
                                                      const internal::v1::DrainKeeperRequest* q,
                                                      internal::v1::DrainKeeperResponse* r)
{
    return dynamicCall(c, q, r);
}
grpc::ServerUnaryReactor* ClusterService::JoinKeeper(grpc::CallbackServerContext* c,
                                                     const internal::v1::JoinKeeperRequest* q,
                                                     internal::v1::JoinKeeperResponse* r)
{
    return dynamicCall(c, q, r);
}
grpc::ServerUnaryReactor* ClusterService::AbandonKeeper(grpc::CallbackServerContext* c,
                                                        const internal::v1::AbandonKeeperRequest* q,
                                                        internal::v1::AbandonKeeperResponse* r)
{
    return dynamicCall(c, q, r);
}
grpc::ServerUnaryReactor* ClusterService::ListMembers(grpc::CallbackServerContext* c,
                                                      const internal::v1::ListMembersRequest* q,
                                                      internal::v1::ListMembersResponse* r)
{
    return dynamicCall(c, q, r);
}
} // namespace chronolog::visor
