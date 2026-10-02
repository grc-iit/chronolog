#pragma once
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace chronolog::client::detail
{
using TimePoint = std::chrono::system_clock::time_point;
inline absl::Status status(const grpc::Status& s)
{
    return {static_cast<absl::StatusCode>(s.error_code()), s.error_message()};
}
inline absl::Status status(const v1::ItemStatus& s) { return {static_cast<absl::StatusCode>(s.code()), s.message()}; }
inline Hlc decode(const v1::Hlc& h) { return {h.physical_ns(), h.logical()}; }
inline void encode(const Hlc& h, v1::Hlc* p)
{
    p->set_physical_ns(h.physical_ns);
    p->set_logical(h.logical);
}
inline EventId decode(const v1::EventId& p) { return {p.story_id(), p.writer_id(), p.incarnation(), p.sequence()}; }
inline void encode(const EventId& id, v1::EventId* p)
{
    p->set_story_id(id.story_id);
    p->set_writer_id(id.writer_id);
    p->set_incarnation(id.incarnation);
    p->set_sequence(id.sequence);
}
inline Route decode(const v1::Route& p)
{
    Route r;
    r.epoch = p.epoch();
    r.grapher = p.grapher();
    r.player = p.player();
    for(const auto& k: p.keepers()) r.keepers.push_back({k.process_id(), k.endpoint()});
    return r;
}
inline Chronicle decode(const v1::Chronicle& p) { return {p.name(), p.tombstoned()}; }
inline Story decode(const v1::Story& p) { return {p.story_id(), p.chronicle(), p.name(), p.epoch(), p.tombstoned()}; }
inline v1::Envelope encode(const Envelope& e)
{
    v1::Envelope p;
    p.set_content_type(e.content_type);
    p.set_payload(e.payload);
    p.set_trace_id(e.trace_id);
    p.set_span_id(e.span_id);
    for(const auto& [k, v]: e.attributes) (*p.mutable_attributes())[k] = v;
    return p;
}
inline Event decode(const v1::Event& p)
{
    Event e;
    e.id = decode(p.id());
    e.hlc = decode(p.hlc());
    e.durability = static_cast<Durability>(p.durability());
    e.physical.physical_ns = p.physical().physical_ns();
    e.physical.status = p.physical().status() == v1::CLOCK_STATUS_SYNCED     ? ClockStatus::Synced
                        : p.physical().status() == v1::CLOCK_STATUS_UNSYNCED ? ClockStatus::Unsynced
                                                                             : ClockStatus::Unavailable;
    if(e.physical.status == ClockStatus::Synced && p.physical().has_uncertainty_ns())
        e.physical.uncertainty_ns = p.physical().uncertainty_ns();
    else if(e.physical.status == ClockStatus::Synced)
        e.physical.status = ClockStatus::Unsynced;
    e.envelope = {p.envelope().content_type(),
                  p.envelope().payload(),
                  p.envelope().trace_id(),
                  p.envelope().span_id(),
                  {}};
    for(const auto& [k, v]: p.envelope().attributes()) e.envelope.attributes[k] = v;
    return e;
}
inline Completion decode(const v1::Completion& p)
{
    Completion c;
    c.complete = p.complete();
    c.frontier = decode(p.frontier());
    c.reason = static_cast<IncompleteReason>(p.reason());
    for(const auto& f: p.laggards()) c.laggards.push_back({f.writer_id(), f.incarnation(), decode(f.frontier())});
    return c;
}
inline bool retryable(const absl::Status& s)
{
    return s.code() == absl::StatusCode::kUnavailable || s.code() == absl::StatusCode::kDeadlineExceeded;
}
// Same channel policy as src/chrono-common/rpc/Channel.h, kept as its own copy because the SDK cannot
// include src/. A lookup that fails or a peer that returns on a new address is re-resolved within a second
// instead of the 30 s gRPC default, reconnects are bounded, and keepalive notices a vanished peer.
inline grpc::ChannelArguments channelPolicy()
{
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, 1000);
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 1000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 1000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    return args;
}
struct State
{
    explicit State(ClientOptions value)
        : options(std::move(value))
        , clock(options.time_source)
    {}
    const ClientOptions options;
    ChronoClock clock;
    std::mutex mutex;
    Hlc floor;
    std::map<StoryId, Route> routes;
    std::map<std::string, std::shared_ptr<grpc::Channel>> channels;
    std::unique_ptr<v1::Catalog::Stub> catalog;
    TimePoint deadline(Deadline d) const { return d.value_or(std::chrono::system_clock::now() + options.rpc_timeout); }
    TimePoint attemptDeadline(TimePoint end) const
    {
        return std::min(end, std::chrono::system_clock::now() + options.rpc_timeout);
    }
    std::shared_ptr<grpc::Channel> channel(const std::string& endpoint)
    {
        std::lock_guard lock(mutex);
        auto& result = channels[endpoint];
        if(!result)
        {
            auto args = channelPolicy();
            for(const auto& [key, value]: options.channel_args)
            {
                if(const auto* number = std::get_if<int>(&value))
                    args.SetInt(key, *number);
                else
                    args.SetString(key, std::get<std::string>(value));
            }
            result = grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), args);
            result->GetState(true);
        }
        return result;
    }
    void observe(Hlc h)
    {
        std::lock_guard lock(mutex);
        floor = std::max(floor, h);
    }
    Hlc causalFloor()
    {
        std::lock_guard lock(mutex);
        return floor;
    }
    void route(StoryId id, const Route& r)
    {
        std::lock_guard lock(mutex);
        routes[id] = r;
    }
    absl::StatusOr<std::string> playerEndpoint(StoryId id, TimePoint end)
    {
        if(!options.player_endpoint.empty())
            return options.player_endpoint;
        grpc::ClientContext context;
        context.set_deadline(end);
        v1::GetStoryRequest request;
        request.set_story_id(id);
        v1::GetStoryResponse response;
        auto rpc = catalog->GetStory(&context, request, &response);
        if(!rpc.ok())
            return status(rpc);
        if(auto result = status(response.status()); !result.ok())
            return result;
        const auto& story = response.story();
        if(story.tombstoned())
            return absl::FailedPreconditionError("story is tombstoned");
        if(!story.has_route() || story.route().epoch() != story.epoch() || story.route().player().empty())
            return absl::DataLossError("story has no current Player route");
        auto current = decode(story.route());
        route(id, current);
        return current.player;
    }
};
// Bounds an individual pull even when its deadline precedes the stream's RPC deadline.
class PullDeadline
{
public:
    PullDeadline(std::shared_ptr<grpc::ClientContext> context, TimePoint end)
        : thread_(
                  [this, context = std::move(context), end]
                  {
                      std::unique_lock lock(mutex_);
                      if(!cv_.wait_until(lock, end, [&] { return done_; }))
                      {
                          expired_ = true;
                          context->TryCancel();
                      }
                  })
    {}
    ~PullDeadline()
    {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    bool expired() const { return expired_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_{};
    std::atomic<bool> expired_{};
    std::thread thread_;
};
} // namespace chronolog::client::detail
