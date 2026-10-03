#include "rpc/Channel.h"
#include "chrono-player/adapter/ClusterClient.h"
#include <algorithm>
#include "chrono-player/adapter/Convert.h"

namespace chronolog::player
{
ClusterClient::ClusterClient(std::shared_ptr<grpc::Channel> visor_internal,
                             Process self,
                             std::chrono::milliseconds deadline,
                             TombstoneLookup lookup)
    : stub_(internal::v1::Cluster::NewStub(std::move(visor_internal)))
    , self_(std::move(self))
    , deadline_(deadline)
    , lookup_(std::move(lookup))
{}
ClusterClient::~ClusterClient()
{
    watcher_.request_stop();
    monitor_.request_stop();
    cv_.notify_all();
    if(watcher_.joinable())
        watcher_.join();
    if(monitor_.joinable())
        monitor_.join();
}
void ClusterClient::onRoute(std::function<void(const Route&)> callback)
{
    std::lock_guard lock(mu_);
    on_route_ = std::move(callback);
}
absl::Status ClusterClient::registerSelf() { return refresh(); }
absl::Status ClusterClient::refresh(std::stop_token stop) const
{
    std::unique_lock registration(registration_mu_);
    if(registered_)
        return absl::OkStatus();
    internal::v1::RegisterRequest request;
    request.set_policy_version(PhysicalPolicy{}.version);
    auto* process = request.mutable_process();
    process->set_process_id(self_.id);
    process->set_instance(self_.instance);
    process->set_endpoint(self_.endpoint);
    process->set_role(internal::v1::PROCESS_ROLE_PLAYER);
    grpc::ClientContext context;
    rpc::withTimeout(context, deadline_);
    std::stop_callback cancel(stop, [&] { context.TryCancel(); });
    internal::v1::RegisterResponse response;
    auto status = stub_->Register(&context, request, &response);
    if(!status.ok())
        return absl::UnavailableError("visor register failed: " + status.error_message());
    if(response.status().code())
        return absl::Status(static_cast<absl::StatusCode>(response.status().code()), response.status().message());
    if(response.has_policy())
    {
        const auto& p = response.policy();
        const PhysicalPolicy expected;
        if(p.version() != expected.version || p.acceptance_window_ns() != expected.acceptance_window_ns ||
           p.skew_limit_ns() != expected.skew_limit_ns || p.hlc_lead_ns() != expected.hlc_lead_ns ||
           p.uncertainty_cap_ns() != static_cast<int64_t>(expected.uncertainty_cap_ns))
            return absl::FailedPreconditionError("physical policy mismatch");
        std::lock_guard lock(mu_);
        skew_limit_ns_ = p.skew_limit_ns();
    }
    registered_ = true;
    registration.unlock();
    for(const auto& update: response.routes()) apply(update);
    registration.lock();
    if(!watcher_.joinable())
    {
        watcher_ = std::jthread([this](std::stop_token token) { watch(token); });
        monitor_ = std::jthread([this](std::stop_token token) { monitor(token); });
    }
    return absl::OkStatus();
}
void ClusterClient::apply(const internal::v1::RouteUpdate& update, bool snapshot) const
{
    std::function<void(const Route&)> callback;
    Route learned;
    {
        std::lock_guard lock(mu_);
        const auto story = update.story_id();
        if(update.tombstoned())
        {
            tombstoned_.insert(story);
            routes_.erase(story);
            physical_policy_.erase(story);
            reconcile_.erase(story);
            revision_ = std::max(revision_, update.revision());
            cv_.notify_all();
            return;
        }
        if(tombstoned_.contains(story))
            return;
        const auto prior = routes_.find(story);
        if(update.revision() < route_revisions_[story] || (snapshot && update.revision() < revision_) ||
           (prior != routes_.end() && update.route().epoch() < prior->second.route.epoch))
            return;
        revision_ = std::max(revision_, update.revision());
        route_revisions_[story] = update.revision();
        routes_[story] = convert::fromProto(update);
        physical_policy_[story] = update.physical_policy();
        looked_up_.erase(story);
        learned = routes_[story].route;
        callback = on_route_;
    }
    cv_.notify_all();
    if(callback)
        callback(learned);
}
void ClusterClient::watch(std::stop_token stop) const
{
    auto delay = std::chrono::milliseconds(rpc::kInitialBackoffMs);
    while(!stop.stop_requested())
    {
        grpc::ClientContext context;
        context.set_wait_for_ready(true);
        std::stop_callback cancel(stop, [&] { context.TryCancel(); });
        internal::v1::WatchRoutesRequest request;
        request.set_process_id(self_.id);
        request.set_instance(self_.instance);
        auto reader = stub_->WatchRoutes(&context, request);
        // The wire has no snapshot-end marker. Confirm all retained identities asynchronously.
        if(lookup_)
        {
            std::lock_guard lock(mu_);
            for(const auto& [story, state]: routes_)
            {
                (void)state;
                reconcile_[story] = 0;
            }
            cv_.notify_all();
        }
        internal::v1::WatchRoutesResponse message;
        bool progressed = false;
        while(reader->Read(&message))
        {
            progressed = true;
            internal::v1::RouteUpdate update;
            if(update.ParseFromString(message.SerializeAsString()))
                apply(update, true);
        }
        const auto status = reader->Finish();
        if(status.error_code() == grpc::StatusCode::NOT_FOUND && !stop.stop_requested())
        {
            {
                std::lock_guard lock(registration_mu_);
                registered_ = false;
            }
            (void)refresh(stop);
        }
        if(progressed)
            delay = std::chrono::milliseconds(rpc::kInitialBackoffMs);
        std::unique_lock lock(mu_);
        cv_.wait_for(lock, stop, delay, [] { return false; });
        delay = std::min(delay * 2, std::chrono::milliseconds(rpc::kMaxBackoffMs));
    }
}
void ClusterClient::monitor(std::stop_token stop) const
{
    auto heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(rpc::kKeepaliveTimeMs);
    while(!stop.stop_requested())
    {
        std::vector<StoryId> pending;
        {
            std::unique_lock lock(mu_);
            cv_.wait_for(lock, stop, std::chrono::milliseconds(rpc::kInitialBackoffMs), [] { return false; });
            for(const auto& [story, attempts]: reconcile_)
                if(attempts < 6)
                    pending.push_back(story);
        }
        for(auto story: pending)
        {
            if(stop.stop_requested())
                return;
            auto gone = lookup_(story);
            if(gone.ok() && *gone)
            {
                internal::v1::RouteUpdate update;
                update.set_story_id(story);
                update.set_tombstoned(true);
                apply(update);
            }
            std::lock_guard lock(mu_);
            auto it = reconcile_.find(story);
            if(it != reconcile_.end() && (gone.ok() || ++it->second >= 6))
                reconcile_.erase(it);
        }
        if(stop.stop_requested())
            return;
        if(std::chrono::steady_clock::now() < heartbeat)
            continue;
        heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(rpc::kKeepaliveTimeMs);
        grpc::ClientContext context;
        rpc::withTimeout(context, deadline_);
        std::stop_callback cancel(stop, [&] { context.TryCancel(); });
        internal::v1::HeartbeatRequest request;
        request.set_process_id(self_.id);
        request.set_instance(self_.instance);
        internal::v1::HeartbeatResponse response;
        auto status = stub_->Heartbeat(&context, request, &response);
        bool unknown = status.error_code() == grpc::StatusCode::NOT_FOUND ||
                       (status.ok() && response.status().code() == static_cast<int>(absl::StatusCode::kNotFound));
        if(status.ok() && response.status().code() == static_cast<int>(absl::StatusCode::kFailedPrecondition))
        {
            // Dynamic heartbeats conflate obsolete and missing registrations; never replace a newer instance.
            grpc::ClientContext listing;
            rpc::withTimeout(listing, deadline_);
            std::stop_callback cancel_listing(stop, [&] { listing.TryCancel(); });
            internal::v1::MembershipResponse members;
            auto listed = stub_->ListMembers(&listing, internal::v1::ListMembersRequest{}, &members);
            unknown = listed.ok() && members.status().code() == 0 &&
                      std::none_of(members.members().begin(),
                                   members.members().end(),
                                   [&](const auto& member) { return member.process().process_id() == self_.id; });
        }
        if(unknown && !stop.stop_requested())
        {
            {
                std::lock_guard lock(registration_mu_);
                registered_ = false;
            }
            (void)refresh(stop);
        }
    }
}
absl::StatusOr<Route> ClusterClient::route(StoryId story) const
{
    auto state = routeState(story);
    if(!state.ok())
        return state.status();
    return state->route;
}
absl::StatusOr<RouteState> ClusterClient::routeState(StoryId story) const
{
    const auto until = std::chrono::system_clock::now() + deadline_;
    {
        std::lock_guard lock(mu_);
        if(tombstoned_.contains(story))
            return absl::FailedPreconditionError("story is tombstoned");
        if(auto it = routes_.find(story); it != routes_.end())
            return it->second;
    }
    if(auto status = refresh(); !status.ok())
        return status;
    std::unique_lock lock(mu_);
    for(;;)
    {
        if(tombstoned_.contains(story))
            return absl::FailedPreconditionError("story is tombstoned");
        if(auto it = routes_.find(story); it != routes_.end())
            return it->second;
        if(!looking_up_.contains(story) && !looked_up_.contains(story))
        {
            if(!lookup_)
                return absl::NotFoundError("no route for story");
            looking_up_.insert(story);
            lock.unlock();
            auto gone = lookup_(story);
            lock.lock();
            looking_up_.erase(story);
            cv_.notify_all();
            if(gone.ok())
            {
                if(*gone)
                {
                    tombstoned_.insert(story);
                    continue;
                }
                looked_up_[story] = absl::OkStatus();
                continue;
            }
            else if(absl::IsNotFound(gone.status()))
                looked_up_[story] = gone.status();
            else
                return gone.status();
        }
        if(auto it = looked_up_.find(story); it != looked_up_.end() && !it->second.ok())
            return it->second;
        if(cv_.wait_until(lock, until) == std::cv_status::timeout)
            return absl::UnavailableError("waiting for story route snapshot");
    }
}
absl::StatusOr<RouteState>
ClusterClient::routeStateAfter(StoryId story, Epoch epoch, std::chrono::system_clock::time_point deadline) const
{
    std::unique_lock lock(mu_);
    cv_.wait_until(lock,
                   deadline,
                   [&]
                   {
                       auto it = routes_.find(story);
                       return tombstoned_.contains(story) || (it != routes_.end() && it->second.route.epoch > epoch);
                   });
    if(tombstoned_.contains(story))
        return absl::FailedPreconditionError("story is tombstoned");
    if(auto it = routes_.find(story); it != routes_.end())
        return it->second;
    return absl::NotFoundError("no route for story");
}
bool ClusterClient::physicalPolicy(StoryId story) const
{
    std::lock_guard lock(mu_);
    auto it = physical_policy_.find(story);
    return it != physical_policy_.end() && it->second;
}
int64_t ClusterClient::skewLimitNs() const
{
    std::lock_guard lock(mu_);
    return skew_limit_ns_;
}
} // namespace chronolog::player
