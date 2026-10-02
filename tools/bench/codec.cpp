#include "codec.h"

#include <absl/crc/crc32c.h>
#include <google/protobuf/arena.h>

#include "chronolog/internal/v1/internal.pb.h"
#include "chronolog/v1/chronolog.pb.h"

namespace chronolog::bench
{
namespace pb = chronolog::v1;
namespace ipb = chronolog::internal::v1;

const char* messageName(Message message)
{
    switch(message)
    {
        case Message::AppendRequest:
            return "AppendRequest";
        case Message::FetchHotResponse:
            return "FetchHotResponse";
        case Message::TransferChunkRequest:
            return "TransferChunkRequest";
    }
    return "unknown";
}

Workload makeWorkload(Message message, size_t payload_bytes, size_t batch)
{
    Workload workload;
    workload.message = message;
    workload.events.resize(batch);
    for(size_t i = 0; i < batch; ++i)
    {
        auto& event = workload.events[i];
        event.writer_id = 7;
        event.incarnation = 1;
        event.sequence = i + 1;
        event.physical_ns = 1'700'000'000'000'000'000LL + static_cast<int64_t>(i) * 1000;
        event.logical = static_cast<uint32_t>(i % 4);
        event.content_type = "application/octet-stream";
        event.payload.assign(payload_bytes, static_cast<char>('a' + (i % 26)));
    }
    return workload;
}

namespace
{
void fillEnvelope(pb::Envelope* envelope, const PlainEvent& event)
{
    envelope->set_content_type(event.content_type);
    envelope->set_payload(event.payload);
}

void fillEvent(pb::Event* out, uint64_t story, const PlainEvent& event)
{
    out->set_durability(pb::DURABILITY_DURABLE);
    auto* id = out->mutable_id();
    id->set_story_id(story);
    id->set_writer_id(event.writer_id);
    id->set_incarnation(event.incarnation);
    id->set_sequence(event.sequence);
    out->mutable_physical()->set_physical_ns(event.physical_ns);
    out->mutable_physical()->set_status(pb::CLOCK_STATUS_SYNCED);
    out->mutable_physical()->set_uncertainty_ns(500);
    out->mutable_hlc()->set_physical_ns(event.physical_ns);
    out->mutable_hlc()->set_logical(event.logical);
    fillEnvelope(out->mutable_envelope(), event);
}

void fillAppend(pb::AppendRequest* request, const Workload& workload)
{
    request->set_story_id(workload.story_id);
    request->set_epoch(1);
    request->set_durability(pb::DURABILITY_DURABLE);
    request->set_batch_id(1);
    for(const auto& event: workload.events)
    {
        auto* item = request->add_items();
        item->set_writer_id(event.writer_id);
        item->set_incarnation(event.incarnation);
        item->set_sequence(event.sequence);
        item->mutable_physical()->set_physical_ns(event.physical_ns);
        item->mutable_physical()->set_status(pb::CLOCK_STATUS_SYNCED);
        item->mutable_physical()->set_uncertainty_ns(500);
        item->mutable_causal_floor()->set_physical_ns(event.physical_ns);
        fillEnvelope(item->mutable_envelope(), event);
    }
}

void fillFetch(ipb::FetchHotResponse* response, const Workload& workload)
{
    auto* batch = response->mutable_batch();
    for(const auto& event: workload.events) fillEvent(batch->add_events(), workload.story_id, event);
}

// The chunk path: events are serialized into a ChunkPayload, checksummed with CRC32C and carried in one frame.
void fillTransfer(ipb::TransferChunkRequest* request, const Workload& workload)
{
    ipb::ChunkPayload payload;
    for(const auto& event: workload.events) fillEvent(payload.add_events(), workload.story_id, event);
    std::string bytes;
    payload.SerializeToString(&bytes);
    const auto crc = static_cast<uint32_t>(absl::ComputeCrc32c(bytes));
    std::string checksum(4, '\0');
    for(int i = 0; i < 4; ++i) checksum[static_cast<size_t>(i)] = static_cast<char>((crc >> (24 - 8 * i)) & 0xff);
    auto* identity = request->mutable_identity();
    identity->set_chunk_id("bench-chunk");
    identity->set_story_id(workload.story_id);
    request->set_checksum_algorithm(ipb::CHECKSUM_ALGORITHM_CRC32C);
    request->set_offset(0);
    request->set_total_bytes(bytes.size());
    request->set_checksum(checksum);
    request->set_final(true);
    request->set_data(std::move(bytes));
}

bool verifyTransfer(const ipb::TransferChunkRequest& request)
{
    ipb::ChunkPayload payload;
    if(!payload.ParseFromString(request.data()))
        return false;
    uint32_t want = 0;
    for(const unsigned char c: request.checksum()) want = (want << 8) | c;
    return static_cast<uint32_t>(absl::ComputeCrc32c(request.data())) == want;
}

template <typename T>
void build(T*, const Workload&);
template <>
void build(pb::AppendRequest* m, const Workload& w)
{
    fillAppend(m, w);
}
template <>
void build(ipb::FetchHotResponse* m, const Workload& w)
{
    fillFetch(m, w);
}
template <>
void build(ipb::TransferChunkRequest* m, const Workload& w)
{
    fillTransfer(m, w);
}

template <typename T>
bool check(const T&)
{
    return true;
}
template <>
bool check(const ipb::TransferChunkRequest& m)
{
    return verifyTransfer(m);
}

template <typename T>
class ProtobufCase final: public CodecCase
{
public:
    ProtobufCase(const Workload& workload, bool arena)
        : workload_(workload)
        , arena_(arena)
    {
        T message;
        build(&message, workload_);
        message.SerializeToString(&wire_);
    }
    size_t encode() override
    {
        if(arena_)
        {
            google::protobuf::Arena arena;
            auto* message = google::protobuf::Arena::Create<T>(&arena);
            build(message, workload_);
            message->SerializeToString(&buffer_);
        }
        else
        {
            T message;
            build(&message, workload_);
            message.SerializeToString(&buffer_);
        }
        return buffer_.size();
    }
    bool decode() override
    {
        if(arena_)
        {
            google::protobuf::Arena arena;
            auto* message = google::protobuf::Arena::Create<T>(&arena);
            return message->ParseFromString(wire_) && check(*message);
        }
        T message;
        return message.ParseFromString(wire_) && check(message);
    }
    size_t wireBytes() const override { return wire_.size(); }

private:
    Workload workload_;
    bool arena_;
    std::string wire_, buffer_;
};

class ProtobufCodec final: public Codec
{
public:
    const char* name() const override { return "protobuf"; }
    std::unique_ptr<CodecCase> prepare(const Workload& workload, bool arena) const override
    {
        switch(workload.message)
        {
            case Message::AppendRequest:
                return std::make_unique<ProtobufCase<pb::AppendRequest>>(workload, arena);
            case Message::FetchHotResponse:
                return std::make_unique<ProtobufCase<ipb::FetchHotResponse>>(workload, arena);
            case Message::TransferChunkRequest:
                return std::make_unique<ProtobufCase<ipb::TransferChunkRequest>>(workload, arena);
        }
        return nullptr;
    }
};
} // namespace

std::vector<std::unique_ptr<Codec>> codecs()
{
    std::vector<std::unique_ptr<Codec>> all;
    all.push_back(std::make_unique<ProtobufCodec>());
    return all;
}
} // namespace chronolog::bench
