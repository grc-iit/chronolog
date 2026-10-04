#include "chrono-player/adapter/EventConvert.h"

#include <google/protobuf/io/coded_stream.h>

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

using google::protobuf::io::CodedOutputStream;

size_t tagSize(int field) { return CodedOutputStream::VarintSize32(static_cast<uint32_t>(field) << 3); }

// An implicit-presence scalar is written only when it is not zero.
size_t scalarSize(int field, uint64_t value) { return value ? tagSize(field) + CodedOutputStream::VarintSize64(value) : 0; }

size_t enumSize(int field, int value)
{
    return value ? tagSize(field) + CodedOutputStream::VarintSize32SignExtended(value) : 0;
}

size_t lengthDelimitedSize(int field, size_t size)
{
    return tagSize(field) + CodedOutputStream::VarintSize64(size) + size;
}

// A string or bytes field is written only when it is not empty.
size_t bytesSize(int field, size_t size) { return size ? lengthDelimitedSize(field, size) : 0; }

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

size_t encodedSize(const Event& event)
{
    const size_t id = scalarSize(v1::EventId::kStoryIdFieldNumber, event.id.story_id) +
                      scalarSize(v1::EventId::kWriterIdFieldNumber, event.id.writer_id) +
                      scalarSize(v1::EventId::kIncarnationFieldNumber, event.id.incarnation) +
                      scalarSize(v1::EventId::kSequenceFieldNumber, event.id.sequence);
    size_t physical =
            scalarSize(v1::TimeReading::kPhysicalNsFieldNumber, static_cast<uint64_t>(event.physical.physical_ns)) +
            enumSize(v1::TimeReading::kStatusFieldNumber, toProto(event.physical.status));
    // Explicit presence: a set bound is written even when it is zero.
    if(event.physical.uncertainty_ns)
        physical += tagSize(v1::TimeReading::kUncertaintyNsFieldNumber) +
                    CodedOutputStream::VarintSize64(*event.physical.uncertainty_ns);
    const size_t hlc = scalarSize(v1::Hlc::kPhysicalNsFieldNumber, static_cast<uint64_t>(event.hlc.physical_ns)) +
                       scalarSize(v1::Hlc::kLogicalFieldNumber, event.hlc.logical);
    size_t envelope = bytesSize(v1::Envelope::kContentTypeFieldNumber, event.envelope.content_type.size()) +
                      bytesSize(v1::Envelope::kPayloadFieldNumber, event.envelope.payload.size()) +
                      bytesSize(v1::Envelope::kTraceIdFieldNumber, event.envelope.trace_id.size()) +
                      bytesSize(v1::Envelope::kSpanIdFieldNumber, event.envelope.span_id.size());
    // A map entry always carries its key (field 1) and value (field 2), empty or not.
    for(const auto& [key, value]: event.envelope.attributes)
        envelope += lengthDelimitedSize(v1::Envelope::kAttributesFieldNumber,
                                        lengthDelimitedSize(1, key.size()) + lengthDelimitedSize(2, value.size()));
    // toProto sets every submessage, so each is written whatever its size.
    return lengthDelimitedSize(v1::Event::kIdFieldNumber, id) +
           lengthDelimitedSize(v1::Event::kPhysicalFieldNumber, physical) +
           lengthDelimitedSize(v1::Event::kHlcFieldNumber, hlc) +
           lengthDelimitedSize(v1::Event::kEnvelopeFieldNumber, envelope) +
           enumSize(v1::Event::kDurabilityFieldNumber, toProto(event.durability));
}

} // namespace chronolog::player::convert
