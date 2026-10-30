#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include <chronolog_errcode.h>
#include <Crc32c.h>
#include <WalFormat.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;

namespace
{
constexpr std::size_t kSegmentDigits = 20;
constexpr char kSegmentSuffix[] = ".wal";

void putLittleEndian(std::string& out, uint64_t value, int bytes)
{
    for(int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF)); }
}

void putLittleEndian(char* out, uint64_t value, int bytes)
{
    for(int i = 0; i < bytes; ++i) { out[i] = static_cast<char>((value >> (8 * i)) & 0xFF); }
}

uint64_t getLittleEndian(char const* data, int bytes)
{
    uint64_t value = 0;
    for(int i = 0; i < bytes; ++i) { value |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i); }
    return value;
}

// Body builder: reserves room for the frame header, then appends fields
class FrameBuilder
{
public:
    explicit FrameBuilder(wal::RecordType type)
        : frame(wal::kFrameHeaderBytes, '\0')
    {
        frame.push_back(static_cast<char>(type));
    }

    void put(uint64_t value, int bytes) { putLittleEndian(frame, value, bytes); }

    void putBytes(char const* data, std::size_t len) { frame.append(data, len); }

    void putString(std::string const& value)
    {
        checkRoom(4 + value.size());
        put(value.size(), 4);
        frame.append(value);
    }

    // fills in the length and CRC of the body and returns the whole frame
    std::string finish()
    {
        std::size_t const body_size = frame.size() - wal::kFrameHeaderBytes;
        checkRoom(0);
        char const* body = frame.data() + wal::kFrameHeaderBytes;
        putLittleEndian(frame.data(), body_size, 4);
        putLittleEndian(frame.data() + 4, chl::crc32c_extend(0, body, body_size), 4);
        return std::move(frame);
    }

private:
    void checkRoom(std::size_t more) const
    {
        if(frame.size() - wal::kFrameHeaderBytes + more > wal::kMaxBodyBytes)
        {
            throw std::length_error("WalFormat: record body over kMaxBodyBytes");
        }
    }

    std::string frame;
};

// Reads fields in order from a body, first checking that each fits in what is left
class BodyReader
{
public:
    explicit BodyReader(std::string_view body)
        : body(body)
    {}

    bool has(std::size_t bytes) const { return body.size() - pos >= bytes; }

    bool atEnd() const { return pos == body.size(); }

    // the type byte, and false when the body is empty or of another type
    bool takeType(wal::RecordType type)
    {
        if(!has(1) || static_cast<uint8_t>(body[pos]) != static_cast<uint8_t>(type))
        {
            return false;
        }
        ++pos;
        return true;
    }

    bool take(int bytes, uint64_t& value)
    {
        if(!has(bytes))
        {
            return false;
        }
        value = getLittleEndian(body.data() + pos, bytes);
        pos += bytes;
        return true;
    }

    bool takeBytes(std::size_t len, std::string_view& value)
    {
        if(!has(len))
        {
            return false;
        }
        value = body.substr(pos, len);
        pos += len;
        return true;
    }

    bool takeString(std::string& value)
    {
        uint64_t len = 0;
        std::string_view bytes;
        if(!take(4, len) || !takeBytes(len, bytes))
        {
            return false;
        }
        value.assign(bytes.data(), bytes.size());
        return true;
    }

private:
    std::string_view body;
    std::size_t pos = 0;
};

std::string offsetText(uint64_t offset) { return "at offset " + std::to_string(offset); }
} // namespace

std::string wal::encodeHeader(HeaderRecord const& header)
{
    FrameBuilder frame(RecordType::Header);
    frame.putBytes(kMagic, sizeof(kMagic));
    frame.put(header.format_version, 2);
    frame.put(header.segment_number, 8);
    frame.put(header.created_ns, 8);
    frame.putString(header.keeper_endpoint);
    return frame.finish();
}

std::string wal::encodeNames(NamesRecord const& names)
{
    FrameBuilder frame(RecordType::Names);
    frame.put(names.story_id, 8);
    frame.putString(names.chronicle);
    frame.putString(names.story);
    return frame.finish();
}

std::string wal::encodeDropped(DroppedRecord const& dropped)
{
    FrameBuilder frame(RecordType::Dropped);
    frame.put(dropped.story_id, 8);
    return frame.finish();
}

std::size_t wal::EventFrame::size() const { return frame_header.size() + head.size() + record.size() + tail.size(); }

wal::EventFrame wal::encodeEvent(LogEvent const& event, std::vector<EventExtension> const& extensions)
{
    EncodedEventParts parts = encodeLogEvent(event, extensions);
    std::size_t const body_size = 1 + parts.head.size() + parts.record.size() + parts.tail.size();
    if(body_size > kMaxBodyBytes)
    {
        throw std::length_error("WalFormat: event record body over kMaxBodyBytes");
    }

    EventFrame frame;
    frame.head.reserve(1 + parts.head.size());
    frame.head.push_back(static_cast<char>(RecordType::Event));
    frame.head.append(parts.head);
    frame.record = parts.record;
    frame.tail = std::move(parts.tail);

    uint32_t crc = crc32c_extend(0, frame.head.data(), frame.head.size());
    crc = crc32c_extend(crc, frame.record.data(), frame.record.size());
    crc = crc32c_extend(crc, frame.tail.data(), frame.tail.size());
    putLittleEndian(frame.frame_header.data(), body_size, 4);
    putLittleEndian(frame.frame_header.data() + 4, crc, 4);
    return frame;
}

std::string wal::segmentFileName(uint64_t segment)
{
    char name[kSegmentDigits + sizeof(kSegmentSuffix)];
    std::snprintf(name, sizeof(name), "%020llu%s", static_cast<unsigned long long>(segment), kSegmentSuffix);
    return name;
}

std::optional<uint64_t> wal::parseSegmentFileName(std::string const& name)
{
    std::size_t const suffix_length = sizeof(kSegmentSuffix) - 1;
    if(name.size() != kSegmentDigits + suffix_length ||
       name.compare(kSegmentDigits, suffix_length, kSegmentSuffix) != 0)
    {
        return std::nullopt;
    }
    uint64_t segment = 0;
    for(std::size_t i = 0; i < kSegmentDigits; ++i)
    {
        char const c = name[i];
        if(c < '0' || c > '9')
        {
            return std::nullopt;
        }
        uint64_t const digit = c - '0';
        if(segment > (std::numeric_limits<uint64_t>::max() - digit) / 10)
        {
            return std::nullopt;
        }
        segment = segment * 10 + digit;
    }
    return segment;
}

wal::ScanResult wal::scanSegment(std::string_view bytes)
{
    ScanResult result{{}, ScanEnd::EndOfData, 0, {}};
    uint64_t pos = 0;
    while(true)
    {
        result.end_offset = pos;
        uint64_t const left = bytes.size() - pos;
        if(left == 0)
        {
            result.end = ScanEnd::EndOfData;
            return result;
        }
        if(left < 4)
        {
            // too short for a length: the end of the data only if nothing was written there
            bool const zeros = bytes.substr(pos).find_first_not_of('\0') == std::string_view::npos;
            result.end = zeros ? ScanEnd::EndOfData : ScanEnd::BadFrame;
            if(!zeros)
            {
                result.reason = "frame header cut short by the end of the segment " + offsetText(pos);
            }
            return result;
        }

        uint64_t const body_size = getLittleEndian(bytes.data() + pos, 4);
        if(body_size == 0)
        {
            result.end = ScanEnd::EndOfData;
            return result;
        }
        result.end = ScanEnd::BadFrame;
        if(body_size > kMaxBodyBytes)
        {
            result.reason = "body length " + std::to_string(body_size) + " over " + std::to_string(kMaxBodyBytes) +
                            " " + offsetText(pos);
            return result;
        }
        if(left < kFrameHeaderBytes + body_size)
        {
            result.reason = "frame of " + std::to_string(kFrameHeaderBytes + body_size) + " bytes " + offsetText(pos) +
                            " passes the end of the segment (" + std::to_string(left) + " bytes left)";
            return result;
        }
        std::string_view const body = bytes.substr(pos + kFrameHeaderBytes, body_size);
        uint32_t const stored = static_cast<uint32_t>(getLittleEndian(bytes.data() + pos + 4, 4));
        uint32_t const computed = crc32c_extend(0, body.data(), body.size());
        if(stored != computed)
        {
            char crcs[64];
            std::snprintf(crcs, sizeof(crcs), " (stored 0x%08x, computed 0x%08x)", stored, computed);
            result.reason = "CRC mismatch " + offsetText(pos) + crcs;
            return result;
        }

        result.records.push_back(ScannedRecord{static_cast<uint8_t>(body[0]), pos, body});
        pos += kFrameHeaderBytes + body_size;
    }
}

int wal::decodeHeader(std::string_view body, HeaderRecord& header)
{
    header.format_version = 0;
    BodyReader in(body);
    std::string_view magic;
    if(!in.takeType(RecordType::Header) || !in.takeBytes(sizeof(kMagic), magic) ||
       std::memcmp(magic.data(), kMagic, sizeof(kMagic)) != 0)
    {
        return CL_ERR_INVALID_ARG;
    }
    uint64_t version = 0;
    if(!in.take(2, version))
    {
        return CL_ERR_INVALID_ARG;
    }
    header.format_version = static_cast<uint16_t>(version);
    if(version != kFormatVersion)
    {
        return CL_ERR_INVALID_ARG;
    }

    uint64_t segment_number = 0;
    uint64_t created_ns = 0;
    std::string keeper_endpoint;
    if(!in.take(8, segment_number) || !in.take(8, created_ns) || !in.takeString(keeper_endpoint) || !in.atEnd())
    {
        return CL_ERR_INVALID_ARG;
    }
    header.segment_number = segment_number;
    header.created_ns = created_ns;
    header.keeper_endpoint = std::move(keeper_endpoint);
    return CL_SUCCESS;
}

int wal::decodeNames(std::string_view body, NamesRecord& names)
{
    BodyReader in(body);
    uint64_t story_id = 0;
    ChronicleName chronicle;
    StoryName story;
    if(!in.takeType(RecordType::Names) || !in.take(8, story_id) || !in.takeString(chronicle) || !in.takeString(story) ||
       !in.atEnd())
    {
        return CL_ERR_INVALID_ARG;
    }
    names.story_id = story_id;
    names.chronicle = std::move(chronicle);
    names.story = std::move(story);
    return CL_SUCCESS;
}

int wal::decodeDropped(std::string_view body, DroppedRecord& dropped)
{
    BodyReader in(body);
    uint64_t story_id = 0;
    if(!in.takeType(RecordType::Dropped) || !in.take(8, story_id) || !in.atEnd())
    {
        return CL_ERR_INVALID_ARG;
    }
    dropped.story_id = story_id;
    return CL_SUCCESS;
}
