#include "adapter/ClusterService.h"
#include "dynamic/MembershipState.h"

#include <chrono>
#include <algorithm>
#include <deque>
#include <optional>

#include "adapter/Convert.h"
#include "rpc/Channel.h"
#include "adapter/WorkerPool.h"
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

v1::TimeReading nowReading()
{
    v1::TimeReading reading;
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    reading.set_physical_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
    // The Visor has no chrony bound yet, so it reports Unsynced with no uncertainty.
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
                               std::chrono::milliseconds failure_timeout)
    : raft_(raft)
    , pool_(pool)
    , membership_(membership)
    , store_(store)
    , ledger_(ledger)
    , feed_(feed)
    , failure_timeout_(failure_timeout)
{
    if(raft_)
        route_notifications_ = std::jthread(
                [this](std::stop_token stop)
                {
                    uint64_t generation = raft_->appliedStore().snapshotGeneration();
                    while(!stop.stop_requested())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        std::set<std::shared_ptr<Stream>> streams;
                        {
                            std::lock_guard lock(mutex_);
                            if(closed_)
                                return;
                            streams = streams_;
                        }
                        if(raft_->leaderLease())
                        {
                            std::vector<std::string> failed;
                            {
                                std::lock_guard heartbeat_lock(heartbeat_mutex_);
                                auto now = std::chrono::steady_clock::now();
                                // A term has one leader, so a lease lapse inside it keeps this term's liveness.
                                if(leader_term_ != raft_->term())
                                {
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
                                internal::v1::CatalogCommand command;
                                command.mutable_membership()->mutable_drain()->set_process_id(id);
                                (void)raft_->propose(command);
                            }
                        }
                        auto current = raft_->appliedStore().snapshotGeneration();
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

grpc::ServerUnaryReactor* ClusterService::ReadClock(grpc::CallbackServerContext* context,
                                                    const internal::v1::ReadClockRequest* request,
                                                    internal::v1::ReadClockResponse* response)
{
    auto* reactor = context->DefaultReactor();
    if(!raft_)
    {
        reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "clock exchange is not configured"));
        return reactor;
    }
    if(!raft_->leaderLease())
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
        if(!pool_ || !pool_->submit(std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
        return reactor;
    }
    *response->mutable_physical() = nowReading();
    response->set_authority_tick_ns(authorityTickNs());
    reactor->Finish(grpc::Status::OK);
    return reactor;
}

grpc::ServerUnaryReactor* ClusterService::Register(grpc::CallbackServerContext* context,
                                                   const internal::v1::RegisterRequest* request,
                                                   internal::v1::RegisterResponse* response)
{
    if(raft_)
        return dynamicCall(context, request, response, 1);
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
        if(status.ok())
        {
            auto routes = routeSnapshot();
            if(routes.ok())
                for(auto& route: *routes) *response->add_routes() = std::move(route);
        }
        *response->mutable_physical() = nowReading();
        response->set_authority_tick_ns(authorityTickNs());
        reactor->Finish(grpc::Status::OK);
    };
    if(pool_)
    {
        if(!pool_->submit(std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
    }
    else
        task();
    return reactor;
}

grpc::ServerUnaryReactor* ClusterService::Heartbeat(grpc::CallbackServerContext* context,
                                                    const internal::v1::HeartbeatRequest* request,
                                                    internal::v1::HeartbeatResponse* response)
{
    if(raft_)
        return dynamicCall(context, request, response, 2);
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    auto task = [this, request = *request, response, reactor]
    {
        absl::Status status =
                membership_.heartbeat(request.process_id(), request.instance(), request.applied_revision());
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
        *response->mutable_physical() = nowReading();
        response->set_authority_tick_ns(authorityTickNs());
        reactor->Finish(grpc::Status::OK);
    };
    if(request->stories_without_physical_policy().empty() || !pool_)
        task();
    else if(!pool_->submit(std::move(task)))
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
    auto routes = routeSnapshot();
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
    if(raft_)
    {
        auto state = std::make_shared<std::pair<uint64_t, std::deque<internal::v1::WatchRoutesResponse>>>();
        for(const auto& message: snapshot) state->first = std::max(state->first, message.revision());
        pull = [this, state]() -> std::optional<internal::v1::WatchRoutesResponse>
        {
            if(state->second.empty())
            {
                auto revision = raft_->appliedStore().membershipRevision();
                if(!revision.ok() || *revision <= state->first)
                    return std::nullopt;
                auto loaded = raft_->appliedStore().membershipRouteChanges(state->first);
                if(!loaded.ok())
                    return std::nullopt;
                const auto& current = *loaded;
                bool trimmed = state->first < current.route_history_floor();
                const auto& updates = trimmed ? current.routes() : current.route_history();
                for(const auto& update: updates)
                    if(trimmed || update.revision() > state->first)
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
                                                          nullptr,
                                                          nullptr,
                                                          nullptr);
}

grpc::ServerWriteReactor<internal::v1::WatchAcquisitionsResponse>*
ClusterService::WatchAcquisitions(grpc::CallbackServerContext*, const internal::v1::WatchAcquisitionsRequest* request)
{
    if(request->keeper_id().empty())
        return new FailedStream<internal::v1::WatchAcquisitionsResponse>(
                grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "keeper_id is required"));
    // The subscription registers for changes before it reads the ledger, so the
    // snapshot at revision R and the queued changes above R leave no gap.
    auto subscription = feed_.subscribe(ledger_, request->keeper_id());
    if(!subscription.ok())
        return new FailedStream<internal::v1::WatchAcquisitionsResponse>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(subscription.status().message())));
    std::shared_ptr<AcquisitionFeed::Subscription> sub = *subscription;
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
    for(auto& stream: open) stream->shutdown();
}


template <class Request, class Response>
grpc::ServerUnaryReactor* ClusterService::dynamicCall(grpc::CallbackServerContext* context,
                                                      const Request* request,
                                                      Response* response,
                                                      int operation)
{
    auto* reactor = context->DefaultReactor();
    if(!raft_)
    {
        reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "dynamic membership required"));
        return reactor;
    }
    auto task = [this, context, request, response, operation, reactor]
    {
        if(!raft_->leaderLease())
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
            grpc::Status result;
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest>)
                result = stub->Register(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::HeartbeatRequest>)
                result = stub->Heartbeat(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::ExtendCeilingRequest>)
                result = stub->ExtendCeiling(&ctx, *request, response);
            else if constexpr(std::is_same_v<Request, internal::v1::ListMembersRequest>)
                result = stub->ListMembers(&ctx, *request, response);
            else
            {
                if(operation == 4)
                    result = stub->DrainKeeper(&ctx, *request, response);
                else if(operation == 5)
                    result = stub->JoinKeeper(&ctx, *request, response);
                else
                    result = stub->AbandonKeeper(&ctx, *request, response);
            }
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
                *q->mutable_heartbeat() = *request;
            else if constexpr(std::is_same_v<Request, internal::v1::ExtendCeilingRequest>)
                *q->mutable_extend() = *request;
            else
            {
                if(operation == 4)
                    *q->mutable_drain() = *request;
                else if(operation == 5)
                    *q->mutable_join() = *request;
                else
                    *q->mutable_abandon() = *request;
            }
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
                if(!dynamic::heartbeatChanges(state, *request))
                {
                    internal::v1::HeartbeatResponse plain;
                    auto status = absl::FailedPreconditionError("obsolete or unknown process instance");
                    for(const auto& m: state.members())
                        if(m.process().process_id() == request->process_id() &&
                           m.process().instance() == request->instance())
                            status = absl::OkStatus();
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
                if constexpr(std::is_same_v<Request, internal::v1::KeeperRequest>)
                {
                    if(operation == 5)
                        heartbeats_[request->process_id()] = std::chrono::steady_clock::now();
                }
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
                if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest> ||
                             std::is_same_v<Request, internal::v1::KeeperRequest>)
                {
                    auto current = dynamic::snapshot(raft_->appliedStore());
                    response->clear_routes();
                    for(const auto& route: current.routes()) *response->add_routes() = route;
                    if constexpr(std::is_same_v<Request, internal::v1::KeeperRequest>)
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
                        heartbeats_[request->process_id()] = std::chrono::steady_clock::now();
                    }
                }
            }
            if constexpr(std::is_same_v<Request, internal::v1::RegisterRequest> ||
                         std::is_same_v<Request, internal::v1::HeartbeatRequest>)
            {
                *response->mutable_physical() = nowReading();
                response->set_authority_tick_ns(authorityTickNs());
            }
        }
        reactor->Finish(grpc::Status::OK);
    };
    if(!pool_ || !pool_->submit(std::move(task)))
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
    return reactor;
}
grpc::ServerUnaryReactor* ClusterService::ExtendCeiling(grpc::CallbackServerContext* c,
                                                        const internal::v1::ExtendCeilingRequest* q,
                                                        internal::v1::ExtendCeilingResponse* r)
{
    return dynamicCall(c, q, r, 3);
}
grpc::ServerUnaryReactor* ClusterService::DrainKeeper(grpc::CallbackServerContext* c,
                                                      const internal::v1::KeeperRequest* q,
                                                      internal::v1::MembershipResponse* r)
{
    return dynamicCall(c, q, r, 4);
}
grpc::ServerUnaryReactor* ClusterService::JoinKeeper(grpc::CallbackServerContext* c,
                                                     const internal::v1::KeeperRequest* q,
                                                     internal::v1::MembershipResponse* r)
{
    return dynamicCall(c, q, r, 5);
}
grpc::ServerUnaryReactor* ClusterService::AbandonKeeper(grpc::CallbackServerContext* c,
                                                        const internal::v1::KeeperRequest* q,
                                                        internal::v1::MembershipResponse* r)
{
    return dynamicCall(c, q, r, 6);
}
grpc::ServerUnaryReactor* ClusterService::ListMembers(grpc::CallbackServerContext* c,
                                                      const internal::v1::ListMembersRequest* q,
                                                      internal::v1::MembershipResponse* r)
{
    return dynamicCall(c, q, r, 7);
}
} // namespace chronolog::visor
