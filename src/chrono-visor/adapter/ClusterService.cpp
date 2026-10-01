#include "adapter/ClusterService.h"

#include <chrono>
#include <deque>
#include <optional>

#include "adapter/Convert.h"
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
                               WorkerPool* pool)
    : raft_(raft)
    , pool_(pool)
    , membership_(membership)
    , store_(store)
    , ledger_(ledger)
    , feed_(feed)
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

grpc::ServerUnaryReactor* ClusterService::Register(grpc::CallbackServerContext* context,
                                                   const internal::v1::RegisterRequest* request,
                                                   internal::v1::RegisterResponse* response)
{
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    if(raft_ && !raft_->leaderLease())
    {
        auto task = [this, context, request, response, reactor]()
        {
            auto endpoint = raft_->leaderEndpoint(true);
            if(endpoint.empty() || raft_->isLocalLeader())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease"));
                return;
            }
            grpc::ClientContext ctx;
            ctx.set_deadline(std::min(context->deadline(), std::chrono::system_clock::now() + std::chrono::seconds(3)));
            auto stub =
                    internal::v1::Cluster::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
            reactor->Finish(stub->Register(&ctx, *request, response));
        };
        if(!pool_ || !pool_->submit(std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
        return reactor;
    }
    auto process = convert::fromProto(request->process());
    absl::Status status = process.ok() ? membership_.registerProcess(*process) : process.status();
    *response->mutable_status() = convert::toProto(status);
    if(status.ok())
    {
        auto routes = routeSnapshot();
        if(routes.ok())
            for(auto& route: *routes) *response->add_routes() = std::move(route);
    }
    *response->mutable_physical() = nowReading();
    response->set_authority_tick_ns(authorityTickNs());
    reactor->Finish(grpc::Status::OK);
    return reactor;
}

grpc::ServerUnaryReactor* ClusterService::Heartbeat(grpc::CallbackServerContext* context,
                                                    const internal::v1::HeartbeatRequest* request,
                                                    internal::v1::HeartbeatResponse* response)
{
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    if(raft_ && !raft_->leaderLease())
    {
        auto task = [this, context, request, response, reactor]()
        {
            auto endpoint = raft_->leaderEndpoint(true);
            if(endpoint.empty() || raft_->isLocalLeader())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no leader lease"));
                return;
            }
            grpc::ClientContext ctx;
            ctx.set_deadline(std::min(context->deadline(), std::chrono::system_clock::now() + std::chrono::seconds(3)));
            auto stub =
                    internal::v1::Cluster::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
            reactor->Finish(stub->Heartbeat(&ctx, *request, response));
        };
        if(!pool_ || !pool_->submit(std::move(task)))
            reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "cluster overloaded"));
        return reactor;
    }
    absl::Status status =
            membership_.heartbeat(request->process_id(), request->instance(), request->applied_revision());
    *response->mutable_status() = convert::toProto(status);
    *response->mutable_physical() = nowReading();
    response->set_authority_tick_ns(authorityTickNs());
    reactor->Finish(grpc::Status::OK);
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
        *message.mutable_route() = std::move(*update.mutable_route());
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
                auto current = ledger_.snapshotAcquisitions();
                if(!current.ok() || current->revision <= state->first)
                    return std::nullopt;
                auto routes = routeSnapshot();
                if(!routes.ok())
                    return std::nullopt;
                for(const auto& update: *routes)
                {
                    auto& message = state->second.emplace_back();
                    message.set_story_id(update.story_id());
                    message.set_revision(update.revision());
                    *message.mutable_route() = update.route();
                }
                state->first = current->revision;
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

} // namespace chronolog::visor
