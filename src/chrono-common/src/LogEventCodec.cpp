#include <limits>
#include <stdexcept>
#include <utility>

#include <LogEventCodec.h>

namespace chl = chronolog;

namespace
{
// version, story id, time, client id, index, record length
constexpr std::size_t kHeadBytes = 1 + 8 + 8 + 8 + 4 + 4;
constexpr std::size_t kExtensionCountBytes = 2;
constexpr std::size_t kExtensionHeadBytes = 2 + 4;

void putLittleEndian(std::string& out, uint64_t value, int bytes)
{
    for(int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF)); }
}

uint64_t getLittleEndian(char const* data, int bytes)
{
    uint64_t value = 0;
    for(int i = 0; i < bytes; ++i) { value |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i); }
    return value;
}

// Reads fields in order, first checking that each fits in what is left
class Reader
{
public:
    Reader(char const* data, std::size_t len)
        : data(data)
        , len(len)
    {}

    bool has(std::size_t bytes) const { return len - pos >= bytes; }

    std::size_t offset() const { return pos; }

    uint64_t take(int bytes)
    {
        uint64_t value = getLittleEndian(data + pos, bytes);
        pos += bytes;
        return value;
    }

    char const* skip(std::size_t bytes)
    {
        char const* start = data + pos;
        pos += bytes;
        return start;
    }

private:
    char const* data;
    std::size_t len;
    std::size_t pos = 0;
};
} // namespace

chl::EncodedEventParts chl::encodeLogEvent(LogEvent const& event, std::vector<EventExtension> const& extensions)
{
    if(extensions.size() > std::numeric_limits<uint16_t>::max())
    {
        throw std::length_error("LogEventCodec: more than 65535 extensions");
    }
    std::size_t total = kHeadBytes + kExtensionCountBytes;
    if(total + event.logRecord.size() > kMaxEncodedEventBytes)
    {
        throw std::length_error("LogEventCodec: encoded event over kMaxEncodedEventBytes");
    }
    total += event.logRecord.size();
    for(EventExtension const& extension: extensions)
    {
        if(total + kExtensionHeadBytes + extension.value.size() > kMaxEncodedEventBytes)
        {
            throw std::length_error("LogEventCodec: encoded event over kMaxEncodedEventBytes");
        }
        total += kExtensionHeadBytes + extension.value.size();
    }

    EncodedEventParts parts;
    parts.head.reserve(kHeadBytes);
    putLittleEndian(parts.head, kLogEventCodecVersion, 1);
    putLittleEndian(parts.head, event.storyId, 8);
    putLittleEndian(parts.head, event.eventTime, 8);
    putLittleEndian(parts.head, event.clientId, 8);
    putLittleEndian(parts.head, event.eventIndex, 4);
    putLittleEndian(parts.head, event.logRecord.size(), 4);

    parts.record = event.logRecord;

    parts.tail.reserve(total - kHeadBytes - event.logRecord.size());
    putLittleEndian(parts.tail, extensions.size(), 2);
    for(EventExtension const& extension: extensions)
    {
        putLittleEndian(parts.tail, extension.tag, 2);
        putLittleEndian(parts.tail, extension.value.size(), 4);
        parts.tail.append(extension.value);
    }
    return parts;
}

chl::CodecStatus
chl::decodeLogEvent(char const* data, std::size_t len, LogEvent& event, std::vector<EventExtension>* extensions)
{
    Reader in(data, len);
    if(!in.has(1))
    {
        return CodecStatus::Truncated;
    }
    if(in.take(1) != kLogEventCodecVersion)
    {
        return CodecStatus::UnsupportedVersion;
    }
    if(!in.has(kHeadBytes - 1))
    {
        return CodecStatus::Truncated;
    }
    uint64_t const story_id = in.take(8);
    uint64_t const time = in.take(8);
    uint64_t const client_id = in.take(8);
    uint32_t const index = static_cast<uint32_t>(in.take(4));
    std::size_t const record_length = in.take(4);
    // a length is judged before the bytes it declares are looked for; the
    // extension count always follows the record
    if(in.offset() + record_length + kExtensionCountBytes > kMaxEncodedEventBytes)
    {
        return CodecStatus::TooLarge;
    }
    if(!in.has(record_length))
    {
        return CodecStatus::Truncated;
    }
    char const* record = in.skip(record_length);

    if(!in.has(kExtensionCountBytes))
    {
        return CodecStatus::Truncated;
    }
    std::size_t const extension_count = in.take(2);
    std::vector<EventExtension> decoded;
    if(extensions != nullptr)
    {
        decoded.reserve(extension_count);
    }
    for(std::size_t i = 0; i < extension_count; ++i)
    {
        if(!in.has(kExtensionHeadBytes))
        {
            return CodecStatus::Truncated;
        }
        uint16_t const tag = static_cast<uint16_t>(in.take(2));
        std::size_t const value_length = in.take(4);
        if(in.offset() + value_length > kMaxEncodedEventBytes)
        {
            return CodecStatus::TooLarge;
        }
        if(!in.has(value_length))
        {
            return CodecStatus::Truncated;
        }
        char const* value = in.skip(value_length);
        if(extensions != nullptr)
        {
            decoded.push_back(EventExtension{tag, std::string(value, value_length)});
        }
    }

    event.storyId = story_id;
    event.eventTime = time;
    event.clientId = client_id;
    event.eventIndex = index;
    event.logRecord.assign(record, record_length);
    if(extensions != nullptr)
    {
        *extensions = std::move(decoded);
    }
    return CodecStatus::Ok;
}
