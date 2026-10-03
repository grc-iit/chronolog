#include "adapter/RouteRead.h"

#include "adapter/Convert.h"
#include "rpc/Channel.h"

namespace chronolog::keeper
{
absl::StatusOr<ConfigMembership::RouteRead> readStoryRoute(v1::Catalog::StubInterface& catalog, StoryId story)
{
    grpc::ClientContext context;
    rpc::withTimeout(context, std::chrono::seconds(2));
    v1::GetStoryRequest request;
    request.set_story_id(story);
    v1::GetStoryResponse response;
    auto status = catalog.GetStory(&context, request, &response);
    if(!status.ok())
        return absl::Status(static_cast<absl::StatusCode>(status.error_code()), status.error_message());
    if(response.status().code() != 0)
        return absl::Status(static_cast<absl::StatusCode>(response.status().code()), response.status().message());
    if(response.story().tombstoned())
        return ConfigMembership::RouteRead{{}, true};
    if(!response.story().has_route() || response.story().route().epoch() != response.story().epoch())
        return absl::UnavailableError("Catalog returned no coherent route");
    return ConfigMembership::RouteRead{convert::fromProto(response.story().route()), false};
}
} // namespace chronolog::keeper
