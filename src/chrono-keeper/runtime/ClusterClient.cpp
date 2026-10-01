#include "runtime/ClusterClient.h"

#include <iostream>
#include <set>

#include "adapter/Convert.h"
#include "wal/WalJournal.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

namespace
{

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
    iv1::RegisterRequest request;
    request.set_recovered_instance(options_.recovered_instance);
    request.set_policy_version(journal_.hasPhysicalPolicy() ? PhysicalPolicy{}.version : 0);
    auto* process = request.mutable_process();
    process->set_process_id(options_.process_id);
    process->set_instance(options_.instance);
    process->set_endpoint(options_.endpoint);
    process->set_role(iv1::PROCESS_ROLE_KEEPER);
    iv1::RegisterResponse response;
    if(auto rpc = invoke(
               [&](auto& stub, auto& context)
               {
                   response.Clear();
                   return stub.Register(&context, request, &response);
               });
       !rpc.ok())
        return toStatus(rpc);
    if(auto status = toStatus(response.status()); !status.ok())
        return status;
    if(!response.visor_replicas().empty())
    {
        std::lock_guard lock(rpc_mutex_);
        replicas_.clear();
        for(const auto& endpoint: response.visor_replicas())
            replicas_.push_back(
                    iv1::Cluster::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials())));
    }
    if(response.has_policy())
    {
        const auto& policy = response.policy();
        const PhysicalPolicy expected;
        if(policy.version() != expected.version || policy.acceptance_window_ns() != expected.acceptance_window_ns ||
           policy.skew_limit_ns() != expected.skew_limit_ns || policy.hlc_lead_ns() != expected.hlc_lead_ns ||
           policy.uncertainty_cap_ns() < 0 ||
           static_cast<uint64_t>(policy.uncertainty_cap_ns()) != expected.uncertainty_cap_ns)
            return absl::FailedPreconditionError("physical policy constants differ");
    }
    if(auto* wal = dynamic_cast<WalJournal*>(&journal_))
        if(auto status = wal->recordInstance(options_.instance); !status.ok())
            return status;
    if(response.has_policy() && response.policy().ceiling_ahead_ns() > 0)
        journal_.enableDynamic(options_.instance,
                               convert::fromProto(response.ceiling_floor()),
                               response.physical_ceiling_floor_ns(),
                               response.policy().acceptance_budget_ns(),
                               response.policy().hlc_budget_ns());
    applyRoutes(response.routes());
    if(journal_.dynamic())
    {
        auto status = extendNow();
        if(!status.ok())
            return status;
    }
    registered_ = true;
    return absl::OkStatus();
}

absl::Status ClusterClient::heartbeatNow()
{
    iv1::HeartbeatRequest request;
    request.set_process_id(options_.process_id);
    request.set_instance(options_.instance);
    request.set_applied_revision(acquisitions_.appliedRevision());
    request.set_applied_route_revision(journal_.appliedRouteRevision());
    std::set<StoryId> stories;
    for(auto story: journal_.storyIds()) stories.insert(story);
    for(StoryId story: stories)
    {
        auto view = journal_.sealedView(story);
        if(!view.ok())
            continue;
        auto* entry = request.add_story_frontiers();
        entry->set_story_id(story);
        *entry->mutable_sealed_frontier() = convert::toProto(view->sealed);
        *entry->mutable_evicted_below() = convert::toProto(journal_.evictionFloor(story));
        for(const auto& owner: journal_.predecessorOwners(story))
            if(journal_.evictionFloor(story) >= owner.own_cut)
            {
                auto* drain = request.add_story_frontiers();
                drain->set_story_id(story);
                drain->set_drained_instance(owner.instance);
                drain->set_drained_epoch(owner.epoch);
                *drain->mutable_sealed_frontier() = convert::toProto(view->sealed);
                *drain->mutable_evicted_below() = convert::toProto(journal_.evictionFloor(story));
            }
        if(auto* wal = dynamic_cast<WalJournal*>(&journal_))
        {
            auto chunks = wal->sealedChunks();
            std::sort(chunks.begin(),
                      chunks.end(),
                      [](const auto& a, const auto& b) { return a.chunk.start < b.chunk.start; });
            std::optional<Hlc> start;
            Hlc through;
            for(const auto& chunk: chunks)
            {
                if(chunk.chunk.story_id != story)
                    continue;
                if(!chunk.settled)
                    break;
                if(!start)
                {
                    start = chunk.chunk.start;
                    through = *start;
                }
                if(chunk.chunk.start > through)
                    break;
                through = std::max(through, chunk.chunk.end);
            }
            if(start)
            {
                auto* proof = entry->mutable_settlement();
                proof->set_instance(options_.instance);
                *proof->mutable_coverage_start() = convert::toProto(*start);
                *proof->mutable_settled_through() = convert::toProto(through);
                *proof->mutable_first_event() = convert::toProto(wal->firstEvent(story).value_or(Hlc{}));
            }
        }
        for(const auto& frontier: view->frontiers) *entry->add_frontiers() = convert::toProto(frontier);
    }
    iv1::HeartbeatResponse response;
    if(auto rpc = invoke(
               [&](auto& stub, auto& context)
               {
                   response.Clear();
                   return stub.Heartbeat(&context, request, &response);
               });
       !rpc.ok())
        return toStatus(rpc);
    applyRoutes(response.routes());
    auto status = toStatus(response.status());
    if(status.code() == absl::StatusCode::kNotFound)
        registered_ = false;
    return status;
}

void ClusterClient::applyRoutes(const google::protobuf::RepeatedPtrField<iv1::RouteUpdate>& routes)
{
    uint64_t revision = 0;
    for(const auto& update: routes)
    {
        auto state = convert::routeState(update);
        journal_.applyRoute(update.story_id(),
                            state,
                            std::find(update.observe_floor().begin(),
                                      update.observe_floor().end(),
                                      options_.process_id) != update.observe_floor().end(),
                            update.revision(),
                            [&] { membership_.setRouteState(update.story_id(), state); });
        revision = std::max(revision, update.revision());
    }
    journal_.acknowledgeRoutes(revision);
}
absl::Status ClusterClient::extendNow()
{
    for(int attempt = 0; attempt < 3; ++attempt)
    {
        iv1::ExtendCeilingRequest request;
        request.set_process_id(options_.process_id);
        request.set_instance(options_.instance);
        request.set_applied_route_revision(journal_.appliedRouteRevision());
        *request.mutable_wanted_hlc() = convert::toProto(journal_.wantedCeiling());
        request.set_realtime_ns(journal_.realtime());
        iv1::ExtendCeilingResponse response;
        auto rpc = invoke(
                [&](auto& stub, auto& context)
                {
                    response.Clear();
                    return stub.ExtendCeiling(&context, request, &response);
                });
        if(!rpc.ok())
            return toStatus(rpc);
        auto status = toStatus(response.status());
        if(status.ok())
        {
            journal_.extendCeiling(convert::fromProto(response.ceiling()), response.physical_ceiling_ns());
            return status;
        }
        if(response.routes().empty())
            return status;
        applyRoutes(response.routes());
        journal_.acknowledgeRoutes(response.fence_revision());
    }
    return absl::UnavailableError("ceiling fence kept changing");
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
        if(registered_ && journal_.dynamic())
        {
            (void)extendNow();
            wait = std::min(wait, 500ms);
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, stop, wait, [this] { return kicked_; });
        kicked_ = false;
    }
}

} // namespace chronolog::keeper
