#include "chrono-player/adapter/EventConvert.h"

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

} // namespace chronolog::player::convert
