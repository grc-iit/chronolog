#include "chrono-player/adapter/ClusterClient.h"
#include <algorithm>
#include "chrono-player/adapter/Convert.h"

namespace chronolog::player
{

ClusterClient::ClusterClient(std::shared_ptr<grpc::Channel> visor_internal,
                             Process self,
                             std::chrono::milliseconds deadline)
    : stub_(internal::v1::Cluster::NewStub(std::move(visor_internal)))
    , self_(std::move(self))
    , deadline_(deadline)
{}

void ClusterClient::onRoute(std::function<void(const Route&)> callback)
{
    std::lock_guard lk(mu_);
    on_route_ = std::move(callback);
}

absl::Status ClusterClient::registerSelf() { return refresh(); }

absl::Status ClusterClient::refresh() const
{
    internal::v1::RegisterRequest request;
    request.set_policy_version(PhysicalPolicy{}.version);
    auto* process = request.mutable_process();
    process->set_process_id(self_.id);
    process->set_instance(self_.instance);
    process->set_endpoint(self_.endpoint);
    process->set_role(internal::v1::PROCESS_ROLE_PLAYER);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + deadline_);
    context.set_wait_for_ready(true);
    internal::v1::RegisterResponse response;
    grpc::Status rpc = stub_->Register(&context, request, &response);
    if(!rpc.ok())
        return absl::UnavailableError("visor register failed: " + rpc.error_message());
    if(response.status().code() != 0)
        return absl::Status(static_cast<absl::StatusCode>(response.status().code()), response.status().message());

    if(response.has_policy())
    {
        const auto& p = response.policy();
        const PhysicalPolicy expected;
        if(p.version() != expected.version || p.acceptance_window_ns() != expected.acceptance_window_ns ||
           p.skew_limit_ns() != expected.skew_limit_ns || p.hlc_lead_ns() != expected.hlc_lead_ns ||
           p.uncertainty_cap_ns() != static_cast<int64_t>(expected.uncertainty_cap_ns))
            return absl::FailedPreconditionError("physical policy mismatch");
    }

    std::vector<Route> learned;
    std::function<void(const Route&)> callback;
    {
        std::lock_guard lk(mu_);
        uint64_t snapshot_revision = 0;
        for(const auto& update: response.routes()) snapshot_revision = std::max(snapshot_revision, update.revision());
        if(snapshot_revision < revision_)
            return absl::OkStatus();
        dynamic_ |= response.has_policy();
        if(response.has_policy())
            skew_limit_ns_ = response.policy().skew_limit_ns();
        for(const auto& update: response.routes())
        {
            auto prior = routes_.find(update.story_id());
            if(update.revision() < route_revisions_[update.story_id()] ||
               (prior != routes_.end() && update.route().epoch() < prior->second.route.epoch))
                continue;
            revision_ = std::max(revision_, update.revision());
            route_revisions_[update.story_id()] = update.revision();
            routes_[update.story_id()] = convert::fromProto(update);
            physical_policy_[update.story_id()] = update.physical_policy();
            learned.push_back(routes_[update.story_id()].route);
        }
        callback = on_route_;
    }
    if(callback)
        for(const auto& route: learned) callback(route);
    return absl::OkStatus();
}

absl::StatusOr<Route> ClusterClient::route(StoryId story) const
{
    {
        std::lock_guard lk(mu_);
        if(auto it = routes_.find(story); it != routes_.end())
            return it->second.route;
    }
    if(auto status = refresh(); !status.ok())
        return status;
    std::lock_guard lk(mu_);
    if(auto it = routes_.find(story); it != routes_.end())
        return it->second.route;
    return absl::NotFoundError("no route for story");
}

absl::StatusOr<RouteState> ClusterClient::routeState(StoryId story) const
{
    bool refresh_dynamic;
    {
        std::lock_guard lk(mu_);
        refresh_dynamic = dynamic_;
    }
    if(refresh_dynamic)
    {
        auto status = refresh();
        if(!status.ok())
            return status;
    }
    auto r = route(story);
    if(!r.ok())
        return r.status();
    std::lock_guard lk(mu_);
    return routes_.at(story);
}
bool ClusterClient::physicalPolicy(StoryId story) const
{
    std::lock_guard lk(mu_);
    auto it = physical_policy_.find(story);
    return it != physical_policy_.end() && it->second;
}
int64_t ClusterClient::skewLimitNs() const
{
    std::lock_guard lk(mu_);
    return skew_limit_ns_;
}

} // namespace chronolog::player
