#pragma once

#include <grpcpp/grpcpp.h>

#include "adapter/WorkerPool.h"
#include "chronolog/metadata_store.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace chronolog::visor
{

// chronolog.v1.Catalog over a MetadataStore. Whole-request failures use gRPC codes
// (INVALID_ARGUMENT for malformed input, UNAVAILABLE for storage failure). Every
// domain failure is an ItemStatus in a gRPC OK response (W10.3). Store calls run on
// the worker pool so SQLite's fsync never blocks a gRPC thread (M11.7).
class CatalogService final: public v1::Catalog::CallbackService
{
public:
    CatalogService(MetadataStore& store, WorkerPool& pool);

    grpc::ServerUnaryReactor* CreateChronicle(grpc::CallbackServerContext* context,
                                              const v1::CreateChronicleRequest* request,
                                              v1::ChronicleResponse* response) override;
    grpc::ServerUnaryReactor* GetChronicle(grpc::CallbackServerContext* context, const v1::GetChronicleRequest* request,
                                           v1::ChronicleResponse* response) override;
    grpc::ServerUnaryReactor* ListChronicles(grpc::CallbackServerContext* context,
                                             const v1::ListChroniclesRequest* request,
                                             v1::ListChroniclesResponse* response) override;
    grpc::ServerUnaryReactor* DestroyChronicle(grpc::CallbackServerContext* context,
                                               const v1::DestroyChronicleRequest* request,
                                               v1::StatusResponse* response) override;
    grpc::ServerUnaryReactor* CreateStory(grpc::CallbackServerContext* context, const v1::CreateStoryRequest* request,
                                          v1::StoryResponse* response) override;
    grpc::ServerUnaryReactor* GetStory(grpc::CallbackServerContext* context, const v1::GetStoryRequest* request,
                                       v1::StoryResponse* response) override;
    grpc::ServerUnaryReactor* ListStories(grpc::CallbackServerContext* context, const v1::ListStoriesRequest* request,
                                          v1::ListStoriesResponse* response) override;
    grpc::ServerUnaryReactor* DestroyStory(grpc::CallbackServerContext* context,
                                           const v1::DestroyStoryRequest* request,
                                           v1::StatusResponse* response) override;
    grpc::ServerUnaryReactor* Acquire(grpc::CallbackServerContext* context, const v1::AcquireRequest* request,
                                      v1::AcquireResponse* response) override;
    grpc::ServerUnaryReactor* Release(grpc::CallbackServerContext* context, const v1::ReleaseRequest* request,
                                      v1::ReleaseResponse* response) override;
    grpc::ServerUnaryReactor* CompareAndSetEpoch(grpc::CallbackServerContext* context,
                                                 const v1::CompareAndSetEpochRequest* request,
                                                 v1::EpochResponse* response) override;

private:
    template <class Fn>
    grpc::ServerUnaryReactor* dispatch(grpc::CallbackServerContext* context, Fn fn);

    MetadataStore& store_;
    WorkerPool& pool_;
};

} // namespace chronolog::visor
