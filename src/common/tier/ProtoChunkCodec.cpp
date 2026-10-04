#include "common/tier/ChunkCodec.h"
#include "common/tier/FileIO.h"
#include "chronolog/v1/chronolog.pb.h"


namespace chronolog
{
namespace
{
constexpr std::size_t kMaxRecord = 16 * 1024 * 1024;
constexpr std::size_t kMaxBytes = 256 * 1024 * 1024;
constexpr std::size_t kMaxEvents = 65536;

v1::Event Encode(const Event& event)
{
    v1::Event result;
    auto* id = result.mutable_id();
    id->set_story_id(event.id.story_id);
    id->set_writer_id(event.id.writer_id);
    id->set_incarnation(event.id.incarnation);
    id->set_sequence(event.id.sequence);
    auto* physical = result.mutable_physical();
    physical->set_physical_ns(event.physical.physical_ns);
    if(event.physical.uncertainty_ns)
        physical->set_uncertainty_ns(*event.physical.uncertainty_ns);
    switch(event.physical.status)
    {
        case ClockStatus::Synced:
            physical->set_status(v1::CLOCK_STATUS_SYNCED);
            break;
        case ClockStatus::Unsynced:
            physical->set_status(v1::CLOCK_STATUS_UNSYNCED);
            break;
        case ClockStatus::Unavailable:
            physical->set_status(v1::CLOCK_STATUS_UNAVAILABLE);
            break;
    }
    result.mutable_hlc()->set_physical_ns(event.hlc.physical_ns);
    result.mutable_hlc()->set_logical(event.hlc.logical);
    auto* envelope = result.mutable_envelope();
    envelope->set_content_type(event.envelope.content_type);
    envelope->set_payload(event.envelope.payload);
    envelope->set_trace_id(event.envelope.trace_id);
    envelope->set_span_id(event.envelope.span_id);
    for(const auto& [key, value]: event.envelope.attributes) (*envelope->mutable_attributes())[key] = value;
    result.set_durability(static_cast<v1::Durability>(event.durability));
    return result;
}

absl::StatusOr<Event> Decode(const v1::Event& event)
{
    if(!event.has_id() || !event.has_hlc() || !v1::Durability_IsValid(event.durability()) ||
       !v1::ClockStatus_IsValid(event.physical().status()))
        return absl::UnavailableError("invalid archived Event");
    Event result;
    result.id = {event.id().story_id(), event.id().writer_id(), event.id().incarnation(), event.id().sequence()};
    result.hlc = {event.hlc().physical_ns(), event.hlc().logical()};
    result.physical.physical_ns = event.physical().physical_ns();
    if(event.physical().has_uncertainty_ns())
        result.physical.uncertainty_ns = event.physical().uncertainty_ns();
    switch(event.physical().status())
    {
        case v1::CLOCK_STATUS_SYNCED:
            result.physical.status = ClockStatus::Synced;
            break;
        case v1::CLOCK_STATUS_UNSYNCED:
            result.physical.status = ClockStatus::Unsynced;
            break;
        default:
            result.physical.status = ClockStatus::Unavailable;
            break;
    }
    result.envelope.content_type = event.envelope().content_type();
    result.envelope.payload = event.envelope().payload();
    result.envelope.trace_id = event.envelope().trace_id();
    result.envelope.span_id = event.envelope().span_id();
    for(const auto& [key, value]: event.envelope().attributes()) result.envelope.attributes[key] = value;
    result.durability = static_cast<Durability>(event.durability());
    return result;
}
} // namespace

absl::Status ProtoChunkCodec::write(const std::filesystem::path& file, std::span<const Event> events) const
{
    if(events.size() > kMaxEvents)
        return absl::InvalidArgumentError("too many chunk events");
    tier_detail::Fd fd(::open(file.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC));
    if(fd.get() < 0)
        return tier_detail::IoError("open chunk");
    std::size_t total = 0;
    for(const auto& event: events)
    {
        const auto encoded = Encode(event);
        const auto size = encoded.ByteSizeLong();
        if(size > kMaxRecord || total + size + 5 > kMaxBytes)
            return absl::InvalidArgumentError("chunk byte limit exceeded");
        total += size + 5;
        std::string bytes;
        auto length = static_cast<uint32_t>(size);
        do {
            bytes.push_back(static_cast<char>((length & 0x7f) | (length > 0x7f ? 0x80 : 0)));
            length >>= 7;
        } while(length != 0);
        bytes += encoded.SerializeAsString();
        const auto status = tier_detail::WriteAll(fd.get(), bytes);
        if(!status.ok())
            return status;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::vector<Event>> ProtoChunkCodec::decode(std::span<unsigned char> input) const
{
    if(input.size() > kMaxBytes)
        return absl::UnavailableError("chunk byte limit exceeded");
    std::vector<Event> events;
    std::size_t total = 0;
    size_t offset = 0;
    while(offset < input.size())
    {
        uint32_t length = 0;
        bool terminated = false;
        for(unsigned shift = 0; shift < 35; shift += 7)
        {
            if(offset == input.size())
                return absl::UnavailableError("torn chunk record length");
            const unsigned char byte = input[offset++];
            if(shift == 28 && (byte & 0xf0) != 0)
                return absl::UnavailableError("torn chunk record length");
            length |= static_cast<uint32_t>(byte & 0x7f) << shift;
            if((byte & 0x80) == 0)
            {
                terminated = true;
                break;
            }
        }
        if(!terminated || length > kMaxRecord || total + length + 5 > kMaxBytes || events.size() >= kMaxEvents)
            return absl::UnavailableError("invalid chunk record length");
        total += length + 5;
        if(length > input.size() - offset)
            return absl::UnavailableError("torn chunk record");
        v1::Event encoded;
        if(!encoded.ParseFromArray(input.data() + offset, length))
            return absl::UnavailableError("invalid chunk protobuf");
        offset += length;
        auto event = Decode(encoded);
        if(!event.ok())
            return event.status();
        events.push_back(*std::move(event));
    }
    return events;
}
} // namespace chronolog
