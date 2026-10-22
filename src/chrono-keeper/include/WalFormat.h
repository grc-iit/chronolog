#ifndef CHRONOLOG_WAL_FORMAT_H
#define CHRONOLOG_WAL_FORMAT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <chronolog_types.h>
#include <LogEventCodec.h>

namespace chronolog::wal
{

// The records of the keeper WAL and the scan recovery runs over a segment.
//
// Frame, all integers little-endian: u32 body length | u32 CRC32C of the body |
// body. The body starts with a one-byte record type. Strings are a u32 length
// followed by the bytes.
//   H  magic "CLWAL" | u16 format version | u64 segment number |
//      u64 creation time (ns) | string keeper recording endpoint
//   N  u64 story id | string chronicle | string story
//   E  the LogEventCodec encoding of the event
//   D  u64 story id
// A body length of 0 at a record boundary ends a segment's data: segments are
// pre-zeroed and every real body holds at least its type byte. A body over
// kMaxBodyBytes is corruption.

constexpr char kMagic[5] = {'C', 'L', 'W', 'A', 'L'};
constexpr uint16_t kFormatVersion = 1;
constexpr uint32_t kMaxBodyBytes = 256u << 20;
// u32 body length and u32 CRC32C
constexpr std::size_t kFrameHeaderBytes = 8;

enum class RecordType : uint8_t
{
    Header = 'H',
    Names = 'N',
    Event = 'E',
    Dropped = 'D'
};

struct HeaderRecord
{
    uint16_t format_version;
    uint64_t segment_number;
    uint64_t created_ns;
    std::string keeper_endpoint;
};

struct NamesRecord
{
    StoryId story_id;
    ChronicleName chronicle;
    StoryName story;
};

struct DroppedRecord
{
    StoryId story_id;
};

// Each returns the whole frame. Throws std::length_error when the body would
// exceed kMaxBodyBytes.
std::string encodeHeader(HeaderRecord const&);
std::string encodeNames(NamesRecord const&);
std::string encodeDropped(DroppedRecord const&);

// An event frame in pieces for writev, in this order: frame_header, head,
// record, tail. head is the type byte followed by the codec's head; record
// points into the event's logRecord, so the event must outlive the frame. The
// CRC in frame_header covers head, record and tail.
struct EventFrame
{
    std::array<char, kFrameHeaderBytes> frame_header;
    std::string head;
    std::string_view record;
    std::string tail;

    std::size_t size() const;
};

// Throws std::length_error when the body would exceed kMaxBodyBytes: an event
// the codec accepts at exactly kMaxEncodedEventBytes is one byte too large once
// the type byte is added.
EventFrame encodeEvent(LogEvent const&, std::vector<EventExtension> const& extensions = {});

// "%020llu.wal", so names sort in segment order
std::string segmentFileName(uint64_t segment);
// The segment number of a name segmentFileName produces; nullopt for anything
// else, including "<n>.wal.partial".
std::optional<uint64_t> parseSegmentFileName(std::string const&);

struct ScannedRecord
{
    uint8_t type;          // the raw type byte, which may be one this build does not know
    uint64_t offset;       // of the frame, from the start of the scanned bytes
    std::string_view body; // the whole body, type byte included, into the scanned bytes
};

enum class ScanEnd
{
    EndOfData, // a zero body length at a record boundary, or the end of the bytes
    BadFrame   // a length over kMaxBodyBytes, a frame past the end of the bytes, or a CRC mismatch
};

struct ScanResult
{
    std::vector<ScannedRecord> records; // every whole frame before end_offset
    ScanEnd end;
    uint64_t end_offset; // where the data ends, or the offset of the bad frame
    std::string reason;  // empty for EndOfData
};

// Reads frames from the start of bytes until the data ends or a frame is bad.
// It checks frames only: what an unknown type, a body that does not decode or
// a BadFrame means is for the caller to decide.
ScanResult scanSegment(std::string_view bytes);

// Each takes a scanned body, type byte included, and returns CL_SUCCESS or
// CL_ERR_INVALID_ARG for a wrong type, a body cut short or bytes left over.
// The output is written only on CL_SUCCESS, except format_version: decodeHeader
// sets it to the version found whenever the magic matches, and to 0 otherwise,
// so a CL_ERR_INVALID_ARG with format_version other than 0 and kFormatVersion
// names a version this build cannot read. The fields after an unknown version
// are not read.
int decodeHeader(std::string_view body, HeaderRecord&);
int decodeNames(std::string_view body, NamesRecord&);
int decodeDropped(std::string_view body, DroppedRecord&);
// An E body is passed to the codec without its type byte:
//   decodeLogEvent(body.data() + 1, body.size() - 1, event, &extensions)

} // namespace chronolog::wal

#endif // CHRONOLOG_WAL_FORMAT_H
