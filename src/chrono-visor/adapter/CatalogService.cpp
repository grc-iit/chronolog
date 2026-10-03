#include "adapter/CatalogService.h"

#include <utility>
#include "raft/RaftMetadataStore.h"
#include "catalog/SqliteMetadataStore.h"

#include "adapter/Convert.h"
#include "rpc/Channel.h"

namespace chronolog::visor
{

namespace
{

grpc::Status invalid(const char* message) { return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message); }

// Storage failures fail the whole request. Every other code is a domain result.
bool storageFailure(const absl::Status& status)
{
    return status.code() == absl::StatusCode::kUnavailable || status.code() == absl::StatusCode::kInternal;
}

grpc::Status wholeRequest(const absl::Status& status)
{
    return grpc::Status(status.code() == absl::StatusCode::kInternal ? grpc::StatusCode::INTERNAL
                                                                     : grpc::StatusCode::UNAVAILABLE,
                        std::string(status.message()));
}

// Fills the common ItemStatus and returns the whole-request status.
template <class Response>
grpc::Status finish(const absl::Status& status, Response* response)
{
    if(storageFailure(status))
        return wholeRequest(status);
    *response->mutable_status() = convert::toProto(status);
    return grpc::Status::OK;
}

} // namespace

CatalogService::CatalogService(MetadataStore& store,
                               WorkerPool& pool,
                               RaftMetadataStore* raft,
                               const Membership* membership)
    : membership_(membership)
    , raft_(raft)
    , store_(store)
    , pool_(pool)
{}

absl::Status CatalogService::fillStory(const Story& story, v1::Story* out) const
{
    *out = convert::toProto(story);
    if(story.tombstoned)
        return absl::OkStatus();
    const auto* sqlite = raft_ ? &raft_->appliedStore() : dynamic_cast<const SqliteMetadataStore*>(&store_);
    absl::StatusOr<Route> route = sqlite        ? sqlite->membershipRoute(story.id)
                                  : membership_ ? membership_->route(story.id)
                                                : absl::StatusOr<Route>(absl::NotFoundError("route unavailable"));
    if(!route.ok())
        return route.status();
    *out->mutable_route() = convert::toProto(*route);
    out->set_epoch(route->epoch);
    return absl::OkStatus();
}

// A follower hands the call to the leader over the pooled channel for that endpoint.
template <class Call>
grpc::Status CatalogService::forward(grpc::CallbackServerContext* context, Call call) const
{
    auto endpoint = raft_->leaderEndpoint(true);
    if(endpoint.empty() || raft_->isLocalLeader())
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no Raft leader");
    grpc::ClientContext ctx;
    rpc::forwardingContext(ctx, context->deadline());
    auto stub = v1::Catalog::NewStub(rpc::peerChannel(endpoint));
    return call(*stub, ctx);
}

template <class Fn>
grpc::ServerUnaryReactor* CatalogService::dispatch(grpc::CallbackServerContext* context, Fn fn)
{
    if(raft_)
        context->AddInitialMetadata("chronolog-raft-leader", std::to_string(raft_->leaderId()));
    grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
    if(!SubmitCall(pool_, context, reactor, [reactor, fn = std::move(fn)]() mutable { reactor->Finish(fn()); }))
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "catalog is overloaded"));
    return reactor;
}

grpc::ServerUnaryReactor* CatalogService::CreateChronicle(grpc::CallbackServerContext* context,
                                                          const v1::CreateChronicleRequest* request,
                                                          v1::CreateChronicleResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.CreateChronicle(&ctx, *request, response); });
                        }
                        if(request->name().empty())
                            return invalid("name is required");
                        auto result = store_.createChronicle(request->name());
                        if(result.ok())
                            *response->mutable_chronicle() = convert::toProto(*result);
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::GetChronicle(grpc::CallbackServerContext* context,
                                                       const v1::GetChronicleRequest* request,
                                                       v1::GetChronicleResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.GetChronicle(&ctx, *request, response); });
                        }
                        if(request->name().empty())
                            return invalid("name is required");
                        auto result = store_.getChronicle(request->name());
                        if(result.ok())
                            *response->mutable_chronicle() = convert::toProto(*result);
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::ListChronicles(grpc::CallbackServerContext* context,
                                                         const v1::ListChroniclesRequest* request,
                                                         v1::ListChroniclesResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.ListChronicles(&ctx, *request, response); });
                        }
                        auto result = store_.listChronicles();
                        if(result.ok())
                            for(const auto& chronicle: *result)
                                *response->add_chronicles() = convert::toProto(chronicle);
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::DestroyChronicle(grpc::CallbackServerContext* context,
                                                           const v1::DestroyChronicleRequest* request,
                                                           v1::DestroyChronicleResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.DestroyChronicle(&ctx, *request, response); });
                        }
                        if(request->name().empty())
                            return invalid("name is required");
                        return finish(store_.destroyChronicle(request->name()), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::CreateStory(grpc::CallbackServerContext* context,
                                                      const v1::CreateStoryRequest* request,
                                                      v1::CreateStoryResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.CreateStory(&ctx, *request, response); });
                        }
                        if(request->chronicle().empty() || request->name().empty())
                            return invalid("chronicle and name are required");
                        auto result = store_.createStory(request->chronicle(), request->name());
                        if(result.ok())
                        {
                            if(!raft_ && !membership_ && !dynamic_cast<const SqliteMetadataStore*>(&store_))
                                *response->mutable_story() = convert::toProto(*result);
                            else
                                return finish(fillStory(*result, response->mutable_story()), response);
                        }
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::GetStory(grpc::CallbackServerContext* context,
                                                   const v1::GetStoryRequest* request,
                                                   v1::GetStoryResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.GetStory(&ctx, *request, response); });
                        }
                        if(request->story_id() == 0)
                            return invalid("story_id is required");
                        auto result = store_.getStory(request->story_id());
                        if(result.ok())
                        {
                            if(!raft_ && !membership_ && !dynamic_cast<const SqliteMetadataStore*>(&store_))
                                *response->mutable_story() = convert::toProto(*result);
                            else
                                return finish(fillStory(*result, response->mutable_story()), response);
                        }
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::ListStories(grpc::CallbackServerContext* context,
                                                      const v1::ListStoriesRequest* request,
                                                      v1::ListStoriesResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.ListStories(&ctx, *request, response); });
                        }
                        if(request->chronicle().empty())
                            return invalid("chronicle is required");
                        auto result = store_.listStories(request->chronicle());
                        if(result.ok())
                            for(const auto& story: *result)
                                if(auto status = fillStory(story, response->add_stories()); !status.ok())
                                    return finish(status, response);
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::DestroyStory(grpc::CallbackServerContext* context,
                                                       const v1::DestroyStoryRequest* request,
                                                       v1::DestroyStoryResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.DestroyStory(&ctx, *request, response); });
                        }
                        if(request->story_id() == 0)
                            return invalid("story_id is required");
                        return finish(store_.destroyStory(request->story_id()), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::Acquire(grpc::CallbackServerContext* context,
                                                  const v1::AcquireRequest* request,
                                                  v1::AcquireResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.Acquire(&ctx, *request, response); });
                        }
                        if(request->story_id() == 0 || request->writer_identity().empty())
                            return invalid("story_id and writer_identity are required");
                        auto result = store_.acquire(request->story_id(),
                                                     request->writer_identity(),
                                                     convert::fromAcquireRequest(*request));
                        if(result.ok())
                            *response = convert::toAcquireResponse(*result);
                        if(!result.ok())
                        {
                            convert::acquireRefusal(result.status(), *response);
                            if(response->incarnation())
                            {
                                auto* ledger = dynamic_cast<AcquisitionLedger*>(&store_);
                                if(ledger)
                                {
                                    auto grant = ledger->requestGrant(request->acquire_request_id());
                                    if(grant.ok())
                                    {
                                        response->set_story_id(grant->story_id);
                                        response->set_writer_id(grant->writer_id);
                                    }
                                }
                            }
                        }
                        return finish(result.status(), response);
                    });
}
grpc::ServerUnaryReactor* CatalogService::RenewAcquisitions(grpc::CallbackServerContext* context,
                                                            const v1::RenewAcquisitionsRequest* request,
                                                            v1::RenewAcquisitionsResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.RenewAcquisitions(&ctx, *request, response); });
                        std::vector<RenewAcquisition> tuples;
                        for(const auto& t: request->acquisitions())
                            tuples.push_back({t.story_id(), t.writer_id(), t.incarnation()});
                        auto results = store_.renewAcquisitions(tuples);
                        if(!results.ok())
                            return grpc::Status(static_cast<grpc::StatusCode>(results.status().code()),
                                                std::string(results.status().message()));
                        for(const auto& result: *results)
                        {
                            auto* item = response->add_results();
                            item->mutable_acquisition()->set_story_id(result.acquisition.story_id);
                            item->mutable_acquisition()->set_writer_id(result.acquisition.writer_id);
                            item->mutable_acquisition()->set_incarnation(result.acquisition.incarnation);
                            *item->mutable_status() = convert::toProto(result.status);
                            if(result.lease)
                            {
                                item->mutable_lease()->set_duration_ns(result.lease->duration_ns);
                                item->mutable_lease()->set_remaining_ns(result.lease->remaining_ns);
                            }
                            if(result.termination_cause)
                                item->set_termination_cause(
                                        static_cast<v1::AcquisitionTerminationCause>(*result.termination_cause));
                        }
                        return grpc::Status::OK;
                    });
}

grpc::ServerUnaryReactor* CatalogService::Release(grpc::CallbackServerContext* context,
                                                  const v1::ReleaseRequest* request,
                                                  v1::ReleaseResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.Release(&ctx, *request, response); });
                        }
                        if(request->story_id() == 0 || request->writer_id() == 0 || request->incarnation() == 0)
                            return invalid("story_id, writer_id and incarnation are required");
                        auto result = store_.release(request->story_id(), request->writer_id(), request->incarnation());
                        if(result.ok())
                        {
                            response->set_fenced(result->fenced);
                            response->set_revision(result->revision);
                        }
                        return finish(result.status(), response);
                    });
}

grpc::ServerUnaryReactor* CatalogService::CompareAndSetEpoch(grpc::CallbackServerContext* context,
                                                             const v1::CompareAndSetEpochRequest* request,
                                                             v1::CompareAndSetEpochResponse* response)
{
    return dispatch(context,
                    [this, context, request, response]()
                    {
                        if(raft_ && !raft_->leaderLease())
                        {
                            return forward(context,
                                           [&](auto& stub, auto& ctx)
                                           { return stub.CompareAndSetEpoch(&ctx, *request, response); });
                        }
                        if(request->story_id() == 0 || request->desired() == 0)
                            return invalid("story_id and desired are required");
                        auto result =
                                store_.compareAndSetEpoch(request->story_id(), request->expected(), request->desired());
                        if(result.ok())
                            response->set_epoch(*result);
                        return finish(result.status(), response);
                    });
}

} // namespace chronolog::visor
