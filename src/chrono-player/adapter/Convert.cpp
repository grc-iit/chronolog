#include "chrono-player/adapter/Convert.h"

namespace chronolog::player::convert
{

namespace
{

ClockStatus fromProto(v1::ClockStatus status)
{
    switch(status)
    {
        case v1::CLOCK_STATUS_SYNCED:
            return ClockStatus::Synced;
        case v1::CLOCK_STATUS_UNSYNCED:
            return ClockStatus::Unsynced;
        default:
            return ClockStatus::Unavailable;
    }
}

v1::ClockStatus toProto(ClockStatus status)
{
    switch(status)
    {
        case ClockStatus::Synced:
            return v1::CLOCK_STATUS_SYNCED;
        case ClockStatus::Unsynced:
            return v1::CLOCK_STATUS_UNSYNCED;
        default:
            return v1::CLOCK_STATUS_UNAVAILABLE;
    }
}

Durability fromProto(v1::Durability durability)
{
    switch(durability)
    {
        case v1::DURABILITY_ACCEPTED:
            return Durability::Accepted;
        case v1::DURABILITY_DURABLE:
            return Durability::Durable;
        default:
            return Durability::Unspecified;
    }
}

v1::Durability toProto(Durability durability)
{
    switch(durability)
    {
        case Durability::Accepted:
            return v1::DURABILITY_ACCEPTED;
        case Durability::Durable:
            return v1::DURABILITY_DURABLE;
        default:
            return v1::DURABILITY_UNSPECIFIED;
    }
}

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

Hlc fromProto(const v1::Hlc& hlc) { return Hlc{hlc.physical_ns(), hlc.logical()}; }

v1::Hlc toProto(const Hlc& hlc)
{
    v1::Hlc out;
    out.set_physical_ns(hlc.physical_ns);
    out.set_logical(hlc.logical);
    return out;
}

Event fromProto(const v1::Event& event)
{
    Event out;
    out.id = {event.id().story_id(), event.id().writer_id(), event.id().incarnation(), event.id().sequence()};
    out.physical.physical_ns = event.physical().physical_ns();
    if(event.physical().has_uncertainty_ns())
        out.physical.uncertainty_ns = event.physical().uncertainty_ns();
    out.physical.status = fromProto(event.physical().status());
    out.hlc = fromProto(event.hlc());
    out.envelope.content_type = event.envelope().content_type();
    out.envelope.payload = event.envelope().payload();
    out.envelope.trace_id = event.envelope().trace_id();
    out.envelope.span_id = event.envelope().span_id();
    for(const auto& [key, value]: event.envelope().attributes()) out.envelope.attributes.emplace(key, value);
    out.durability = fromProto(event.durability());
    return out;
}

v1::Event toProto(const Event& event)
{
    v1::Event out;
    auto* id = out.mutable_id();
    id->set_story_id(event.id.story_id);
    id->set_writer_id(event.id.writer_id);
    id->set_incarnation(event.id.incarnation);
    id->set_sequence(event.id.sequence);
    auto* physical = out.mutable_physical();
    physical->set_physical_ns(event.physical.physical_ns);
    if(event.physical.uncertainty_ns)
        physical->set_uncertainty_ns(*event.physical.uncertainty_ns);
    physical->set_status(toProto(event.physical.status));
    *out.mutable_hlc() = toProto(event.hlc);
    auto* envelope = out.mutable_envelope();
    envelope->set_content_type(event.envelope.content_type);
    envelope->set_payload(event.envelope.payload);
    envelope->set_trace_id(event.envelope.trace_id);
    envelope->set_span_id(event.envelope.span_id);
    for(const auto& [key, value]: event.envelope.attributes) (*envelope->mutable_attributes())[key] = value;
    out.set_durability(toProto(event.durability));
    return out;
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
