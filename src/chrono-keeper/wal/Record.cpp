#include "wal/Record.h"

#include <limits>
#include <stdexcept>

#include <absl/crc/crc32c.h>
#include "chronolog/v1/chronolog.pb.h"

namespace chronolog::wal
{
namespace
{
void put32(std::string& bytes, uint32_t value)
{
    for(unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(value >> (8 * i)));
}
} // namespace

uint32_t uint32(std::string_view bytes)
{
    uint32_t value = 0;
    for(unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return value;
}

std::string frame(std::string_view payload)
{
    if(payload.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("WAL record too large");
    std::string out;
    put32(out, static_cast<uint32_t>(payload.size()));
    put32(out, static_cast<uint32_t>(absl::ComputeCrc32c(absl::string_view(payload.data(), payload.size()))));
    out.append(payload);
    return out;
}

std::string encode(const Event& event)
{
    v1::Event proto;
    auto* id = proto.mutable_id();
    id->set_story_id(event.id.story_id);
    id->set_writer_id(event.id.writer_id);
    id->set_incarnation(event.id.incarnation);
    id->set_sequence(event.id.sequence);
    proto.mutable_hlc()->set_physical_ns(event.hlc.physical_ns);
    proto.mutable_hlc()->set_logical(event.hlc.logical);
    auto* time = proto.mutable_physical();
    time->set_physical_ns(event.physical.physical_ns);
    time->set_status(event.physical.status == ClockStatus::Synced     ? v1::CLOCK_STATUS_SYNCED
                     : event.physical.status == ClockStatus::Unsynced ? v1::CLOCK_STATUS_UNSYNCED
                                                                      : v1::CLOCK_STATUS_UNAVAILABLE);
    if(event.physical.uncertainty_ns)
        time->set_uncertainty_ns(*event.physical.uncertainty_ns);
    auto* envelope = proto.mutable_envelope();
    envelope->set_payload(event.envelope.payload);
    envelope->set_content_type(event.envelope.content_type);
    envelope->set_trace_id(event.envelope.trace_id);
    envelope->set_span_id(event.envelope.span_id);
    for(const auto& [key, value]: event.envelope.attributes) (*envelope->mutable_attributes())[key] = value;
    proto.set_durability(v1::DURABILITY_DURABLE);
    return std::string(1, 'E') + proto.SerializeAsString();
}

Event decode(std::string_view payload)
{
    v1::Event proto;
    if(!proto.ParseFromArray(payload.data(), static_cast<int>(payload.size())) ||
       proto.durability() != v1::DURABILITY_DURABLE || proto.id().story_id() == 0 || proto.id().writer_id() == 0 ||
       proto.id().incarnation() == 0 || proto.id().sequence() == 0)
        throw std::runtime_error("invalid WAL event");
    Event event;
    event.id = {proto.id().story_id(), proto.id().writer_id(), proto.id().incarnation(), proto.id().sequence()};
    event.hlc = {proto.hlc().physical_ns(), proto.hlc().logical()};
    const auto& time = proto.physical();
    event.physical.physical_ns = time.physical_ns();
    event.physical.status = time.status() == v1::CLOCK_STATUS_SYNCED     ? ClockStatus::Synced
                            : time.status() == v1::CLOCK_STATUS_UNSYNCED ? ClockStatus::Unsynced
                                                                         : ClockStatus::Unavailable;
    if(time.has_uncertainty_ns())
        event.physical.uncertainty_ns = time.uncertainty_ns();
    const auto& envelope = proto.envelope();
    event.envelope = {envelope.content_type(), envelope.payload(), envelope.trace_id(), envelope.span_id(), {}};
    for(const auto& [key, value]: envelope.attributes()) event.envelope.attributes[key] = value;
    event.durability = Durability::Durable;
    return event;
}

std::string reserve(Hlc hlc)
{
    v1::Hlc proto;
    proto.set_physical_ns(hlc.physical_ns);
    proto.set_logical(hlc.logical);
    return std::string(1, 'R') + proto.SerializeAsString();
}

Hlc decodeReserve(std::string_view payload)
{
    v1::Hlc proto;
    if(!proto.ParseFromArray(payload.data(), static_cast<int>(payload.size())) || proto.physical_ns() < 0)
        throw std::runtime_error("invalid WAL reservation");
    return {proto.physical_ns(), proto.logical()};
}

} // namespace chronolog::wal
