#pragma once

#include <grpcpp/grpcpp.h>

#include "adapter/WorkerPool.h"
#include "chronolog/metadata_store.h"
#include "chronolog/membership.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace chronolog::visor
{
class RaftMetadataStore;

// chronolog.v1.Catalog over a MetadataStore. Whole-request failures use gRPC codes
// (INVALID_ARGUMENT for malformed input, UNAVAILABLE for storage failure). Every
// domain failure is an ItemStatus in a gRPC OK response (W10.3). Store calls run on
// the worker pool so SQLite's fsync never blocks a gRPC thread (M11.7).
class CatalogService final: public v1::Catalog::CallbackService
{
public:
    CatalogService(MetadataStore& store,
                   WorkerPool& pool,
                   RaftMetadataStore* raft = nullptr,
                   const Membership* membership = nullptr);

    grpc::ServerUnaryReactor* CreateChronicle(grpc::CallbackServerContext* context,
                                              const v1::CreateChronicleRequest* request,
                                              v1::CreateChronicleResponse* response) override;
    grpc::ServerUnaryReactor* GetChronicle(grpc::CallbackServerContext* context,
                                           const v1::GetChronicleRequest* request,
                                           v1::GetChronicleResponse* response) override;
    grpc::ServerUnaryReactor* ListChronicles(grpc::CallbackServerContext* context,
                                             const v1::ListChroniclesRequest* request,
                                             v1::ListChroniclesResponse* response) override;
    grpc::ServerUnaryReactor* DestroyChronicle(grpc::CallbackServerContext* context,
                                               const v1::DestroyChronicleRequest* request,
                                               v1::DestroyChronicleResponse* response) override;
    grpc::ServerUnaryReactor* CreateStory(grpc::CallbackServerContext* context,
                                          const v1::CreateStoryRequest* request,
                                          v1::CreateStoryResponse* response) override;
    grpc::ServerUnaryReactor* GetStory(grpc::CallbackServerContext* context,
                                       const v1::GetStoryRequest* request,
                                       v1::GetStoryResponse* response) override;
    grpc::ServerUnaryReactor* ListStories(grpc::CallbackServerContext* context,
                                          const v1::ListStoriesRequest* request,
                                          v1::ListStoriesResponse* response) override;
    grpc::ServerUnaryReactor* DestroyStory(grpc::CallbackServerContext* context,
                                           const v1::DestroyStoryRequest* request,
                                           v1::DestroyStoryResponse* response) override;
    grpc::ServerUnaryReactor* Acquire(grpc::CallbackServerContext* context,
                                      const v1::AcquireRequest* request,
                                      v1::AcquireResponse* response) override;
    grpc::ServerUnaryReactor* Release(grpc::CallbackServerContext* context,
                                      const v1::ReleaseRequest* request,
                                      v1::ReleaseResponse* response) override;
    grpc::ServerUnaryReactor* CompareAndSetEpoch(grpc::CallbackServerContext* context,
                                                 const v1::CompareAndSetEpochRequest* request,
                                                 v1::CompareAndSetEpochResponse* response) override;

private:
    template <class Call>
    grpc::Status forward(grpc::CallbackServerContext* context, Call call) const;
    template <class Fn>
    grpc::ServerUnaryReactor* dispatch(grpc::CallbackServerContext* context, Fn fn);

    absl::Status fillStory(const Story&, v1::Story*) const;
    const Membership* membership_;
    RaftMetadataStore* raft_;
    MetadataStore& store_;
    WorkerPool& pool_;
};

} // namespace chronolog::visor
