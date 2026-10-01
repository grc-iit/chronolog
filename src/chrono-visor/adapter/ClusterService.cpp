#include "adapter/ClusterService.h"

#include <chrono>
#include <deque>
#include <optional>

#include "adapter/Convert.h"

namespace chronolog::visor
{

namespace
{

// Server stream that sends `initial`, then whatever `pull` yields on each wake(),
// and holds open until shutdown or cancellation. The Subscription behind `pull` and
// the service registry own it through shared_ptr, so a late wake() is harmless.
template <class Msg>
class WriteStream final: public grpc::ServerWriteReactor<Msg>,
                         public ClusterService::Stream,
                         public std::enable_shared_from_this<WriteStream<Msg>>
{
public:
    WriteStream(std::deque<Msg> initial, std::function<std::optional<Msg>()> pull, std::function<bool()> failed,
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
    reading.set_status(v1::UNSYNCED);
    return reading;
}

uint64_t authorityTickNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

} // namespace

ClusterService::ClusterService(StaticRouteMembership& membership, const MetadataStore& store,
                               const AcquisitionLedger& ledger, AcquisitionFeed& feed)
    : membership_(membership)
    , store_(store)
    , ledger_(ledger)
    , feed_(feed)
{}

grpc::ServerUnaryReactor* ClusterService::Register(grpc::CallbackServerContext* context,
                                                   const internal::v1::RegisterRequest* request,
                                                   internal::v1::RegisterResponse* response)
{
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    auto process = convert::fromProto(request->process());
    absl::Status status = process.ok() ? membership_.registerProcess(*process) : process.status();
    *response->mutable_status() = convert::toProto(status);
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
    absl::Status status = membership_.heartbeat(request->process_id(), request->instance(), request->applied_revision());
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
    auto stream = std::make_shared<WriteStream<Msg>>(
            std::move(initial), std::move(pull), std::move(failed), [this, cleanup = std::move(cleanup)](Stream* done) {
                if(cleanup)
                    cleanup();
                forget(done);
            });
    streams_.insert(stream);
    if(attach)
        attach([weak = std::weak_ptr<Stream>(stream)]() {
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

grpc::ServerWriteReactor<internal::v1::RouteUpdate>* ClusterService::WatchRoutes(
        grpc::CallbackServerContext*, const internal::v1::RouteSubscription*)
{
    std::deque<internal::v1::RouteUpdate> snapshot;
    auto chronicles = store_.listChronicles();
    if(!chronicles.ok())
        return new FailedStream<internal::v1::RouteUpdate>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(chronicles.status().message())));
    for(const auto& chronicle: *chronicles)
    {
        if(chronicle.tombstoned)
            continue;
        auto stories = store_.listStories(chronicle.name);
        if(!stories.ok())
            return new FailedStream<internal::v1::RouteUpdate>(
                    grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(stories.status().message())));
        for(const auto& story: *stories)
        {
            if(story.tombstoned)
                continue;
            auto route = membership_.route(story.id);
            if(!route.ok())
                continue;
            internal::v1::RouteUpdate update;
            update.set_story_id(story.id);
            *update.mutable_route() = convert::toProto(*route);
            snapshot.push_back(std::move(update));
        }
    }
    return startStream<internal::v1::RouteUpdate>(std::move(snapshot), nullptr, nullptr, nullptr, nullptr);
}

grpc::ServerWriteReactor<internal::v1::AcquisitionUpdate>* ClusterService::WatchAcquisitions(
        grpc::CallbackServerContext*, const internal::v1::AcquisitionSubscription*)
{
    // The subscription carries the ledger snapshot first, then every later change.
    // Each Keeper filters on assigned_keeper, so one stream shape serves all of them.
    auto subscription = feed_.subscribe(ledger_);
    if(!subscription.ok())
        return new FailedStream<internal::v1::AcquisitionUpdate>(
                grpc::Status(grpc::StatusCode::UNAVAILABLE, std::string(subscription.status().message())));
    std::shared_ptr<AcquisitionFeed::Subscription> sub = *subscription;
    auto* reactor = startStream<internal::v1::AcquisitionUpdate>(
            {},
            [sub]() -> std::optional<internal::v1::AcquisitionUpdate> {
                auto change = sub->pop();
                if(!change)
                    return std::nullopt;
                return convert::toProto(*change);
            },
            [sub]() { return sub->overflowed(); }, [sub](std::function<void()> wake) { sub->setWakeup(std::move(wake)); },
            [sub]() { sub->setWakeup(nullptr); });
    return reactor;
}

void ClusterService::shutdown()
{
    std::set<std::shared_ptr<Stream>> open;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        open = streams_;
    }
    for(auto& stream: open)
        stream->shutdown();
}

} // namespace chronolog::visor
