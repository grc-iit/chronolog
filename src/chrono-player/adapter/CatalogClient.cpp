#include "chrono-player/adapter/StoryCatalog.h"

namespace chronolog::player
{

CatalogClient::CatalogClient(std::shared_ptr<grpc::Channel> visor, std::chrono::milliseconds deadline)
    : stub_(v1::Catalog::NewStub(std::move(visor)))
    , deadline_(deadline)
{}

absl::Status CatalogClient::ensureLive(StoryId story) const
{
    v1::GetStoryRequest request;
    request.set_story_id(story);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + deadline_);
    v1::GetStoryResponse response;
    grpc::Status rpc = stub_->GetStory(&context, request, &response);
    if(!rpc.ok())
        return absl::UnavailableError("catalog unavailable: " + rpc.error_message());
    const int code = response.status().code();
    if(code == static_cast<int>(absl::StatusCode::kNotFound))
        return absl::FailedPreconditionError("unknown story");
    if(code != 0)
        return absl::UnavailableError("catalog refused story lookup: " + response.status().message());
    if(response.story().tombstoned())
        return absl::FailedPreconditionError("story is tombstoned");
    return absl::OkStatus();
}

} // namespace chronolog::player
