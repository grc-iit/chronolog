#include "player/adapter/Convert.h"

namespace chronolog::player::convert
{

namespace
{

v1::IncompleteReason toProto(IncompleteReason reason)
{
    switch(reason)
    {
        case IncompleteReason::LaggingWriters:
            return v1::INCOMPLETE_REASON_LAGGING_WRITERS;
        case IncompleteReason::PhysicalAxisUnbounded:
            return v1::INCOMPLETE_REASON_PHYSICAL_AXIS_UNBOUNDED;
        case IncompleteReason::SourceFailed:
            return v1::INCOMPLETE_REASON_SOURCE_FAILED;
        case IncompleteReason::Truncated:
            return v1::INCOMPLETE_REASON_TRUNCATED;
        default:
            return v1::INCOMPLETE_REASON_UNSPECIFIED;
    }
}

} // namespace

RouteState fromProto(const internal::v1::RouteUpdate& update)
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

Route fromProto(const v1::Route& route)
{
    Route out;
    out.epoch = route.epoch();
    for(const auto& keeper: route.keepers()) out.keepers.push_back({keeper.process_id(), keeper.endpoint()});
    out.grapher = route.grapher();
    out.player = route.player();
    return out;
}

v1::Completion toProto(const Completion& completion)
{
    v1::Completion out;
    out.set_complete(completion.complete);
    *out.mutable_frontier() = toProto(completion.frontier);
    for(const auto& laggard: completion.laggards)
    {
        auto* entry = out.add_laggards();
        entry->set_writer_id(laggard.writer_id);
        entry->set_incarnation(laggard.incarnation);
        *entry->mutable_frontier() = toProto(laggard.frontier);
    }
    out.set_reason(toProto(completion.reason));
    return out;
}

absl::StatusOr<Range> rangeFromProto(const v1::ReadRequest& request)
{
    switch(request.range_case())
    {
        case v1::ReadRequest::kHlc:
            return Range{Range::Axis::Hlc, fromProto(request.hlc().start()), fromProto(request.hlc().end())};
        case v1::ReadRequest::kPhysical:
            return Range{Range::Axis::Physical,
                         Hlc{request.physical().start_ns(), 0},
                         Hlc{request.physical().end_ns(), 0}};
        default:
            return absl::InvalidArgumentError("exactly one of hlc or physical range is required");
    }
}

absl::StatusOr<Event> positionFromProto(StoryId story, const v1::TailRequest& request)
{
    if(!request.has_from())
        return absl::InvalidArgumentError("from position is required");
    Event position;
    position.id = {request.from().id().story_id(),
                   request.from().id().writer_id(),
                   request.from().id().incarnation(),
                   request.from().id().sequence()};
    if(position.id.story_id == 0)
        position.id.story_id = story;
    position.hlc = fromProto(request.from().hlc());
    return position;
}

internal::v1::FetchHotRequest fetchHotRequest(StoryId story, const Range& range, uint64_t max_events)
{
    internal::v1::FetchHotRequest request;
    request.set_story_id(story);
    if(range.axis == Range::Axis::Hlc)
    {
        *request.mutable_hlc()->mutable_start() = toProto(range.start);
        *request.mutable_hlc()->mutable_end() = toProto(range.end);
    }
    else
    {
        request.mutable_physical()->set_start_ns(range.start.physical_ns);
        request.mutable_physical()->set_end_ns(range.end.physical_ns);
    }
    request.set_max_events(max_events);
    return request;
}

grpc::Status toGrpc(const absl::Status& status)
{
    if(status.ok())
        return grpc::Status::OK;
    return grpc::Status(static_cast<grpc::StatusCode>(status.raw_code()), std::string(status.message()));
}

} // namespace chronolog::player::convert
