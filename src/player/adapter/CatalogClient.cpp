#include <algorithm>
#include <set>
#include "common/rpc/Channel.h"
#include "player/adapter/StoryCatalog.h"

namespace chronolog::player
{

CatalogClient::CatalogClient(std::shared_ptr<grpc::Channel> visor, std::chrono::milliseconds deadline)
    : deadline_(deadline)
{
    // Connect now, while name resolution works; a later resolver outage then cannot fail a Read that finds
    // the connection up.
    visor->GetState(true);
    stub_ = v1::Catalog::NewStub(std::move(visor));
}

absl::Status CatalogClient::ensureLive(StoryId story) const
{
    auto gone = tombstoned(story);
    if(absl::IsNotFound(gone.status()))
        return absl::FailedPreconditionError("unknown story");
    if(!gone.ok())
        return gone.status();
    return *gone ? absl::FailedPreconditionError("story is tombstoned") : absl::OkStatus();
}

absl::StatusOr<bool> CatalogClient::tombstoned(StoryId story) const
{
    v1::GetStoryRequest request;
    request.set_story_id(story);
    grpc::ClientContext context;
    rpc::withDeadline(context, std::chrono::system_clock::now() + deadline_);
    v1::GetStoryResponse response;
    grpc::Status rpc = stub_->GetStory(&context, request, &response);
    if(!rpc.ok())
        return absl::UnavailableError("catalog unavailable: " + rpc.error_message());
    const int code = response.status().code();
    if(code == static_cast<int>(absl::StatusCode::kNotFound))
        return absl::NotFoundError("unknown story");
    if(code != 0)
        return absl::UnavailableError("catalog refused story lookup: " + response.status().message());
    return response.story().tombstoned();
}

absl::StatusOr<PrefixResolution> CatalogClient::resolvePrefix(const std::string& prefix, uint32_t limit) const
{
    v1::ListStoriesByPrefixRequest request;
    request.set_prefix(prefix);
    request.set_limit(limit);
    grpc::ClientContext context;
    rpc::withDeadline(context, std::chrono::system_clock::now() + deadline_);
    v1::ListStoriesByPrefixResponse response;
    grpc::Status rpc = stub_->ListStoriesByPrefix(&context, request, &response);
    if(!rpc.ok())
        return absl::UnavailableError("catalog unavailable: " + rpc.error_message());
    if(response.status().code() != 0)
        return absl::Status(static_cast<absl::StatusCode>(response.status().code()), response.status().message());
    if(response.limit_exceeded())
        return absl::ResourceExhaustedError("more than " + std::to_string(limit) + " stories under the prefix");
    PrefixResolution resolution{response.revision(), {}};
    for(const auto& story: response.stories()) resolution.stories.push_back(story.story_id());
    return resolution;
}

absl::StatusOr<PrefixConfirmation>
CatalogClient::confirmPrefix(const std::string& prefix, uint32_t limit, const PrefixResolution& resolved) const
{
    auto current = resolvePrefix(prefix, limit);
    if(!current.ok())
        return current.status();
    const std::set<StoryId> known(resolved.stories.begin(), resolved.stories.end());
    // Story ids are never reused (I3.5), so an id the first resolution lacks names a story created after it.
    const bool created = std::any_of(current->stories.begin(),
                                     current->stories.end(),
                                     [&](StoryId story) { return !known.contains(story); });
    return PrefixConfirmation{created, std::move(*current)};
}

} // namespace chronolog::player
