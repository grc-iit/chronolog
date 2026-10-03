#pragma once

#include "chronolog/v1/chronolog.grpc.pb.h"
#include "membership/ConfigMembership.h"

namespace chronolog::keeper
{
absl::StatusOr<ConfigMembership::RouteRead> readStoryRoute(v1::Catalog::StubInterface& catalog, StoryId story);
}
