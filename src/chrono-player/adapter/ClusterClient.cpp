#include "chrono-player/adapter/ClusterClient.h"
#include "chrono-player/adapter/Convert.h"

namespace chronolog::player
{

ClusterClient::ClusterClient(std::shared_ptr<grpc::Channel> visor_internal,
                             Process self,
                             std::chrono::milliseconds deadline,
                             std::chrono::milliseconds refresh_interval)
    : stub_(internal::v1::Cluster::NewStub(std::move(visor_internal)))
    , self_(std::move(self))
    , deadline_(deadline)
    , refresh_interval_(refresh_interval)
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
    auto* process = request.mutable_process();
    process->set_process_id(self_.id);
    process->set_instance(self_.instance);
    process->set_endpoint(self_.endpoint);
    process->set_role(internal::v1::PROCESS_ROLE_PLAYER);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + deadline_);
    internal::v1::RegisterResponse response;
    grpc::Status rpc = stub_->Register(&context, request, &response);
    if(!rpc.ok())
        return absl::UnavailableError("visor register failed: " + rpc.error_message());
    if(response.status().code() != 0)
        return absl::Status(static_cast<absl::StatusCode>(response.status().code()), response.status().message());

    std::vector<Route> learned;
    std::function<void(const Route&)> callback;
    {
        std::lock_guard lk(mu_);
        last_refresh_ = std::chrono::steady_clock::now();
        refreshed_ = true;
        for(const auto& update: response.routes())
        {
            routes_[update.story_id()] = convert::fromProto(update.route());
            learned.push_back(routes_[update.story_id()]);
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
            return it->second;
        if(refreshed_ && std::chrono::steady_clock::now() - last_refresh_ < refresh_interval_)
            return absl::NotFoundError("no route for story");
    }
    if(auto status = refresh(); !status.ok())
        return status;
    std::lock_guard lk(mu_);
    if(auto it = routes_.find(story); it != routes_.end())
        return it->second;
    return absl::NotFoundError("no route for story");
}

} // namespace chronolog::player
