#ifndef CHRONOLOG_STORY_CHUNK_WIRE_FORMAT_H
#define CHRONOLOG_STORY_CHUNK_WIRE_FORMAT_H

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include <StoryChunk.h>

namespace chronolog
{
namespace StoryChunkWireFormat
{
constexpr uint64_t MAGIC = 0x3146574b434c4843ULL; // "CHLCKWF1" in little-endian byte order.
constexpr uint32_t VERSION = 1;

inline void appendBytes(std::string& output, void const* data, std::size_t size)
{
    output.append(static_cast<char const*>(data), size);
}

template <typename T>
inline void appendScalar(std::string& output, T value)
{
    appendBytes(output, &value, sizeof(T));
}

inline void appendString(std::string& output, std::string const& value)
{
    appendScalar<uint64_t>(output, static_cast<uint64_t>(value.size()));
    appendBytes(output, value.data(), value.size());
}

inline bool readBytes(char const*& cursor, char const* end, void* destination, std::size_t size)
{
    if(size > static_cast<std::size_t>(end - cursor))
    {
        return false;
    }
    std::memcpy(destination, cursor, size);
    cursor += size;
    return true;
}

template <typename T>
inline bool readScalar(char const*& cursor, char const* end, T& value)
{
    return readBytes(cursor, end, &value, sizeof(T));
}

inline bool readString(char const*& cursor, char const* end, std::string& value)
{
    uint64_t size = 0;
    if(!readScalar(cursor, end, size) || size > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }
    if(size > static_cast<uint64_t>(end - cursor))
    {
        return false;
    }
    value.assign(cursor, static_cast<std::size_t>(size));
    cursor += static_cast<std::size_t>(size);
    return true;
}

inline std::size_t estimateBytes(StoryChunk const& story_chunk)
{
    std::size_t payload_bytes = 0;
    for(auto const& event: story_chunk)
    {
        payload_bytes += event.second.logRecord.size();
    }
    return payload_bytes + static_cast<std::size_t>(story_chunk.getEventCount()) * 40U +
           story_chunk.getChronicleName().size() + story_chunk.getStoryName().size() + 64U;
}

inline void serialize(StoryChunk const& story_chunk, std::string& output)
{
    output.clear();
    output.reserve(estimateBytes(story_chunk));

    appendScalar<uint64_t>(output, MAGIC);
    appendScalar<uint32_t>(output, VERSION);
    appendString(output, story_chunk.getChronicleName());
    appendString(output, story_chunk.getStoryName());
    appendScalar<uint64_t>(output, story_chunk.getStoryId());
    appendScalar<uint64_t>(output, story_chunk.getStartTime());
    appendScalar<uint64_t>(output, story_chunk.getEndTime());
    appendScalar<uint64_t>(output, static_cast<uint64_t>(story_chunk.getEventCount()));

    for(auto const& event_pair: story_chunk)
    {
        auto const& event = event_pair.second;
        appendScalar<uint64_t>(output, event.time());
        appendScalar<uint64_t>(output, event.getClientId());
        appendScalar<uint32_t>(output, event.index());
        appendString(output, event.logRecord);
    }
}

inline bool hasFastWireMagic(char const* data, std::size_t size)
{
    if(size < sizeof(uint64_t))
    {
        return false;
    }
    uint64_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == MAGIC;
}

inline bool deserialize(char const* data, std::size_t size, StoryChunk& story_chunk)
{
    char const* cursor = data;
    char const* end = data + size;
    uint64_t magic = 0;
    uint32_t version = 0;
    std::string chronicle_name;
    std::string story_name;
    uint64_t story_id = 0;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint64_t event_count = 0;

    if(!readScalar(cursor, end, magic) || magic != MAGIC || !readScalar(cursor, end, version) || version != VERSION ||
       !readString(cursor, end, chronicle_name) || !readString(cursor, end, story_name) ||
       !readScalar(cursor, end, story_id) || !readScalar(cursor, end, start_time) ||
       !readScalar(cursor, end, end_time) || !readScalar(cursor, end, event_count))
    {
        return false;
    }

    StoryChunk decoded(chronicle_name, story_name, story_id, start_time, end_time);
    for(uint64_t i = 0; i < event_count; ++i)
    {
        uint64_t event_time = 0;
        uint64_t client_id = 0;
        uint32_t event_index = 0;
        std::string payload;
        if(!readScalar(cursor, end, event_time) || !readScalar(cursor, end, client_id) ||
           !readScalar(cursor, end, event_index) || !readString(cursor, end, payload))
        {
            return false;
        }
        if(decoded.insertEvent(LogEvent(story_id, event_time, client_id, event_index, std::move(payload))) != 1)
        {
            return false;
        }
    }
    if(cursor != end)
    {
        return false;
    }
    story_chunk = std::move(decoded);
    return true;
}
}
}

#endif
