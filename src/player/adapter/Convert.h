#pragma once

#include <grpcpp/grpcpp.h>
#include "chrono-player/adapter/EventConvert.h"
#include "chronolog/internal/v1/internal.pb.h"
#include "chronolog/replay.h"
#include "chronolog/v1/chronolog.pb.h"

// Conversions between contract types and generated protobuf types. Only the adapters, the
// Keeper client and this file include generated headers.
namespace chronolog::player::convert
{

Route fromProto(const v1::Route& route);
RouteState fromProto(const internal::v1::RouteUpdate& update);
v1::Completion toProto(const Completion& completion);

// A request whose range oneof is unset is INVALID_ARGUMENT (W10.10).
absl::StatusOr<Range> rangeFromProto(const v1::ReadRequest& request);
// Tail resumes after a Position. The Position id may omit story_id, which the request supplies.
absl::StatusOr<Event> positionFromProto(StoryId story, const v1::TailRequest& request);

internal::v1::FetchHotRequest fetchHotRequest(StoryId story, const Range& range, uint64_t max_events);

// absl status codes carry the same integers as grpc and google.rpc codes.
grpc::Status toGrpc(const absl::Status& status);

} // namespace chronolog::player::convert
