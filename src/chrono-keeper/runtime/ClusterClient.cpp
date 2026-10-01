#include "runtime/ClusterClient.h"

#include <iostream>
#include <set>

#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

namespace
{

constexpr std::chrono::seconds kRpcDeadline{5};

void setDeadline(grpc::ClientContext& context)
{
    context.set_deadline(std::chrono::system_clock::now() + kRpcDeadline);
}

absl::Status toStatus(const grpc::Status& status)
{
    return absl::Status(static_cast<absl::StatusCode>(status.error_code()), status.error_message());
}

absl::Status toStatus(const v1::ItemStatus& status)
{
    return absl::Status(static_cast<absl::StatusCode>(status.code()), status.message());
}

} // namespace

ClusterClient::ClusterClient(std::shared_ptr<grpc::Channel> channel,
                             Options options,
                             RamJournal& journal,
                             ConfigMembership& membership,
                             const AcquisitionWatcher& acquisitions)
    : stub_(iv1::Cluster::NewStub(std::move(channel)))
    , options_(std::move(options))
    , journal_(journal)
    , membership_(membership)
    , acquisitions_(acquisitions)
{}

ClusterClient::~ClusterClient()
{
    thread_.request_stop();
    cv_.notify_all();
}

absl::Status ClusterClient::registerNow()
{
    grpc::ClientContext context;
    setDeadline(context);
    iv1::RegisterRequest request;
    auto* process = request.mutable_process();
    process->set_process_id(options_.process_id);
    process->set_instance(options_.instance);
    process->set_endpoint(options_.endpoint);
    process->set_role(iv1::PROCESS_ROLE_KEEPER);
    iv1::RegisterResponse response;
    if(auto rpc = stub_->Register(&context, request, &response); !rpc.ok())
        return toStatus(rpc);
    if(auto status = toStatus(response.status()); !status.ok())
        return status;
    for(const auto& update: response.routes())
        membership_.setRoute(update.story_id(), convert::fromProto(update.route()));
    registered_ = true;
    return absl::OkStatus();
}

absl::Status ClusterClient::heartbeatNow()
{
    grpc::ClientContext context;
    setDeadline(context);
    iv1::HeartbeatRequest request;
    request.set_process_id(options_.process_id);
    request.set_instance(options_.instance);
    request.set_applied_revision(acquisitions_.appliedRevision());
    std::set<StoryId> stories;
    for(const auto& key: journal_.liveWriters()) stories.insert(key.story_id);
    for(StoryId story: stories)
    {
        auto view = journal_.sealedView(story);
        if(!view.ok())
            continue;
        auto* entry = request.add_story_frontiers();
        entry->set_story_id(story);
        *entry->mutable_sealed_frontier() = convert::toProto(view->sealed);
        for(const auto& frontier: view->frontiers) *entry->add_frontiers() = convert::toProto(frontier);
    }
    iv1::HeartbeatResponse response;
    if(auto rpc = stub_->Heartbeat(&context, request, &response); !rpc.ok())
        return toStatus(rpc);
    auto status = toStatus(response.status());
    if(status.code() == absl::StatusCode::kNotFound)
        registered_ = false;
    return status;
}

void ClusterClient::start()
{
    thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void ClusterClient::kick()
{
    {
        std::lock_guard lock(mutex_);
        kicked_ = true;
    }
    cv_.notify_all();
}

void ClusterClient::loop(std::stop_token stop)
{
    using namespace std::chrono_literals;
    auto backoff = 100ms;
    while(!stop.stop_requested())
    {
        std::chrono::milliseconds wait = options_.interval;
        if(!registered_)
        {
            auto status = registerNow();
            if(status.ok())
            {
                backoff = 100ms;
            }
            else
            {
                std::cerr << "chrono_keeper: register failed: " << status << std::endl;
                wait = backoff;
                backoff = std::min<std::chrono::milliseconds>(backoff * 2, 5000ms);
            }
        }
        else if(auto status = heartbeatNow(); !status.ok())
        {
            std::cerr << "chrono_keeper: heartbeat failed: " << status << std::endl;
            wait = 500ms;
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, stop, wait, [this] { return kicked_; });
        kicked_ = false;
    }
}

} // namespace chronolog::keeper
