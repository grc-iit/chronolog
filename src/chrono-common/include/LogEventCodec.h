#ifndef CHRONOLOG_LOG_EVENT_CODEC_H
#define CHRONOLOG_LOG_EVENT_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

// The byte encoding of a LogEvent, used for the keeper WAL's E records.
// Layout, all integers little-endian:
//   u8 codec version | u64 story id | u64 time | u64 client id | u32 index |
//   u32 record length | record | u16 extension count | {u16 tag, u32 length, bytes}*
// New per-event fields become extension tags, so they need no new codec
// version. The codec knows no tags: it returns every extension it reads and
// the caller skips the tags it does not know. The codec does not write the WAL
// record-type byte; the WAL prefixes it.

constexpr uint8_t kLogEventCodecVersion = 1;

// Largest encoded event, version byte through the last extension. The WAL
// treats a record body over 256 MiB as corruption.
constexpr std::size_t kMaxEncodedEventBytes = 256u << 20;

struct EventExtension
{
    uint16_t tag;
    std::string value;
};

struct EncodedEventParts
{
    std::string head;        // through the record length
    std::string_view record; // the event's own bytes, not copied
    std::string tail;        // extension count and extensions
};

// record points into event.logRecord, so event must outlive the parts.
// Throws std::length_error when the encoding would exceed
// kMaxEncodedEventBytes or there are more than 65535 extensions.
EncodedEventParts encodeLogEvent(LogEvent const& event, std::vector<EventExtension> const& extensions = {});

enum class CodecStatus
{
    Ok,
    Truncated,         // data ends before a field it declares
    TooLarge,          // a declared length takes the event over kMaxEncodedEventBytes
    UnsupportedVersion // the first byte is not kLogEventCodecVersion
};

// Decodes one event from the start of data. Bytes after the last extension
// are not read. event and extensions are written only on Ok; extensions may be
// null when the caller wants none of them.
CodecStatus
decodeLogEvent(char const* data, std::size_t len, LogEvent& event, std::vector<EventExtension>* extensions = nullptr);

} // namespace chronolog

#endif // CHRONOLOG_LOG_EVENT_CODEC_H
