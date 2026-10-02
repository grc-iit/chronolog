#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chronolog::bench
{
enum class Message
{
    AppendRequest,
    FetchHotResponse,
    TransferChunkRequest
};

const char* messageName(Message message);

// A codec neutral description of one wire message, so every codec encodes identical content.
struct PlainEvent
{
    uint64_t writer_id{}, incarnation{}, sequence{};
    int64_t physical_ns{};
    uint32_t logical{};
    std::string content_type;
    std::string payload;
};

struct Workload
{
    Message message{Message::AppendRequest};
    uint64_t story_id{1};
    std::vector<PlainEvent> events;
};

Workload makeWorkload(Message message, size_t payload_bytes, size_t batch);

// One prepared codec and workload pair. encode() builds the codec's native message from the workload
// and serializes it; decode() parses the bytes encode() produced into a fresh native message.
class CodecCase
{
public:
    virtual ~CodecCase() = default;
    virtual size_t encode() = 0;
    virtual bool decode() = 0;
    virtual size_t wireBytes() const = 0;
};

// A research codec plugs in with one subclass of Codec and one of CodecCase, then one line in codecs().
class Codec
{
public:
    virtual ~Codec() = default;
    virtual const char* name() const = 0;
    virtual std::unique_ptr<CodecCase> prepare(const Workload&, bool arena) const = 0;
};

std::vector<std::unique_ptr<Codec>> codecs();
} // namespace chronolog::bench
