#ifndef CHRONOLOG_STORY_CHUNK_SERIALIZER_H
#define CHRONOLOG_STORY_CHUNK_SERIALIZER_H

#include <cstdint>
#include <cstring>
#include <string>

#include "StoryChunk.h"
#include "chronolog_types.h"

namespace chronolog {

/// Binary serialization format for StoryChunk ↔ CTE blob.
///
/// Layout:
///   [u32 chronicle_name_len][bytes chronicle_name]
///   [u32 story_name_len][bytes story_name]
///   [u64 story_id][u64 start_time][u64 end_time][u64 revision_time]
///   [u32 event_count]
///   For each event (sorted by EventSequence):
///     [u64 event_time][u64 client_id][u32 event_index]
///     [u32 record_len][bytes log_record]

class StoryChunkSerializer {
 public:
  static size_t computeSerializedSize(const StoryChunk& chunk) {
    size_t size = 0;
    // chronicle_name
    size += sizeof(uint32_t) + chunk.getChronicleName().size();
    // story_name
    size += sizeof(uint32_t) + chunk.getStoryName().size();
    // story_id, start_time, end_time, revision_time
    size += 4 * sizeof(uint64_t);
    // event_count
    size += sizeof(uint32_t);
    // events
    for (auto it = chunk.begin(); it != chunk.end(); ++it) {
      const LogEvent& ev = it->second;
      // event_time + client_id + event_index + record_len + record
      size += sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) +
              sizeof(uint32_t) + ev.logRecord.size();
    }
    return size;
  }

  static size_t serialize(const StoryChunk& chunk, char* buf,
                          size_t buf_size) {
    size_t needed = computeSerializedSize(chunk);
    if (buf_size < needed) return 0;

    size_t off = 0;

    // chronicle_name
    writeString(buf, off, chunk.getChronicleName());
    // story_name
    writeString(buf, off, chunk.getStoryName());
    // story_id, start_time, end_time
    writeU64(buf, off, chunk.getStoryId());
    writeU64(buf, off, chunk.getStartTime());
    writeU64(buf, off, chunk.getEndTime());
    // revision_time — use end_time as proxy (not directly accessible)
    writeU64(buf, off, chunk.getEndTime());
    // event_count
    writeU32(buf, off, static_cast<uint32_t>(chunk.getEventCount()));

    // events
    for (auto it = chunk.begin(); it != chunk.end(); ++it) {
      const LogEvent& ev = it->second;
      writeU64(buf, off, ev.eventTime);
      writeU64(buf, off, ev.clientId);
      writeU32(buf, off, ev.eventIndex);
      writeString(buf, off, ev.logRecord);
    }

    return off;
  }

  static StoryChunk* deserialize(const char* buf, size_t buf_size) {
    size_t off = 0;

    std::string chronicle_name = readString(buf, buf_size, off);
    if (off > buf_size) return nullptr;

    std::string story_name = readString(buf, buf_size, off);
    if (off > buf_size) return nullptr;

    uint64_t story_id = readU64(buf, buf_size, off);
    uint64_t start_time = readU64(buf, buf_size, off);
    uint64_t end_time = readU64(buf, buf_size, off);
    uint64_t revision_time = readU64(buf, buf_size, off);
    (void)revision_time;

    if (off + sizeof(uint32_t) > buf_size) return nullptr;
    uint32_t event_count = readU32(buf, buf_size, off);

    auto* chunk = new StoryChunk(chronicle_name, story_name, story_id,
                                 start_time, end_time);

    for (uint32_t i = 0; i < event_count; ++i) {
      if (off + sizeof(uint64_t) * 2 + sizeof(uint32_t) > buf_size) break;

      uint64_t event_time = readU64(buf, buf_size, off);
      uint64_t client_id = readU64(buf, buf_size, off);
      uint32_t event_index = readU32(buf, buf_size, off);
      std::string record = readString(buf, buf_size, off);
      if (off > buf_size) break;

      LogEvent ev(story_id, event_time, client_id, event_index, record);
      chunk->insertEvent(ev);
    }

    return chunk;
  }

  /// Build blob name from story chunk metadata.
  static std::string makeBlobName(const std::string& story_name,
                                  uint64_t start_time, uint64_t end_time) {
    return story_name + "." + std::to_string(start_time) + "." +
           std::to_string(end_time);
  }

  /// Parse start_time from a blob name of format "story.start.end".
  static bool parseBlobName(const std::string& blob_name,
                            std::string& story_name, uint64_t& start_time,
                            uint64_t& end_time) {
    // Find last two dots
    size_t dot2 = blob_name.rfind('.');
    if (dot2 == std::string::npos || dot2 == 0) return false;
    size_t dot1 = blob_name.rfind('.', dot2 - 1);
    if (dot1 == std::string::npos || dot1 == 0) return false;

    story_name = blob_name.substr(0, dot1);
    try {
      start_time = std::stoull(blob_name.substr(dot1 + 1, dot2 - dot1 - 1));
      end_time = std::stoull(blob_name.substr(dot2 + 1));
    } catch (...) {
      return false;
    }
    return true;
  }

 private:
  static void writeU32(char* buf, size_t& off, uint32_t val) {
    memcpy(buf + off, &val, sizeof(val));
    off += sizeof(val);
  }

  static void writeU64(char* buf, size_t& off, uint64_t val) {
    memcpy(buf + off, &val, sizeof(val));
    off += sizeof(val);
  }

  static void writeString(char* buf, size_t& off, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    writeU32(buf, off, len);
    memcpy(buf + off, s.data(), len);
    off += len;
  }

  static uint32_t readU32(const char* buf, size_t buf_size, size_t& off) {
    if (off + sizeof(uint32_t) > buf_size) {
      off = buf_size + 1;
      return 0;
    }
    uint32_t val;
    memcpy(&val, buf + off, sizeof(val));
    off += sizeof(val);
    return val;
  }

  static uint64_t readU64(const char* buf, size_t buf_size, size_t& off) {
    if (off + sizeof(uint64_t) > buf_size) {
      off = buf_size + 1;
      return 0;
    }
    uint64_t val;
    memcpy(&val, buf + off, sizeof(val));
    off += sizeof(val);
    return val;
  }

  static std::string readString(const char* buf, size_t buf_size,
                                size_t& off) {
    uint32_t len = readU32(buf, buf_size, off);
    if (off + len > buf_size) {
      off = buf_size + 1;
      return "";
    }
    std::string s(buf + off, len);
    off += len;
    return s;
  }
};

}  // namespace chronolog

#endif  // CHRONOLOG_STORY_CHUNK_SERIALIZER_H
