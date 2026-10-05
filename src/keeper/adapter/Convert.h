#pragma once

#include <vector>

#include <grpcpp/support/status.h>

#include "chronolog/internal/v1/internal.pb.h"
#include "chronolog/types.h"
#include "common/predicate/Predicate.h"
#include "chronolog/v1/chronolog.pb.h"

// Conversions between contract types and generated protobuf types. The service adapters,
// the cluster client and the route watcher are the only Keeper code that includes
// generated headers.
namespace chronolog::keeper::convert
{

// absl status codes carry the same integers as google.rpc.Code.
v1::ItemStatus toProto(const absl::Status& status);
v1::Hlc toProto(const Hlc& hlc);
Hlc fromProto(const v1::Hlc& hlc);
v1::Route toProto(const Route& route);
Route fromProto(const v1::Route& route);
template <class Update>
RouteState routeState(const Update& update)
{
    RouteState state;
    state.route = fromProto(update.route());
    state.ordering_cut = fromProto(update.ordering_cut());
    state.physical_floor = update.physical_floor_ns();
    state.archived_below = fromProto(update.archived_below());
    for(const auto& p: update.predecessors())
        state.predecessors.push_back({{p.keeper().process_id(), p.keeper().endpoint()},
                                      p.instance(),
                                      p.epoch(),
                                      fromProto(p.own_cut()),
                                      p.own_physical_ceiling_ns()});
    for(const auto& r: update.abandoned())
        state.abandoned.push_back({Range::Axis::Hlc, fromProto(r.start()), fromProto(r.end())});
    return state;
}
v1::Event toProto(const Event& event);
v1::AppendResult toProto(const AppendResult& result);
v1::WriterFrontier toProto(const Frontier& frontier);

// ClockStatus UNSPECIFIED maps to Unavailable. A bound is kept only for Synced, and a Synced
// reading without a bound is demoted to Unsynced.
TimeReading fromProto(const v1::TimeReading& reading);
Envelope fromProto(const v1::Envelope& envelope);
AppendItem fromProto(const v1::AppendItem& item);
EventPredicate fromProto(const v1::Predicate& predicate);

struct ParsedAppend
{
    AppendBatch batch;
    Durability durability{Durability::Unspecified};
    uint64_t batch_id{};
};

// INVALID_ARGUMENT for a missing story or epoch, an empty batch or an unknown durability.
absl::StatusOr<ParsedAppend> parse(const v1::AppendRequest& request);
absl::StatusOr<ParsedAppend> parse(const v1::AppendStreamRequest& request);

// Fills results in request order. current_route is set at the top level when every item
// failed with a route newer than the epoch the request presented (I4.1).
void fill(v1::AppendResponse& response, const ParsedAppend& parsed, const std::vector<AppendResult>& results);
void fill(v1::AppendStreamResponse& response, const ParsedAppend& parsed, const std::vector<AppendResult>& results);

// Whole-request failure to a gRPC status.
grpc::Status toGrpc(const absl::Status& status);

} // namespace chronolog::keeper::convert
