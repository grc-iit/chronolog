// Unit tests for WalFormat, the record frames of the keeper WAL and the scan
// recovery runs over each segment. A scan must return every whole record, stop
// at the first zero length, and report a frame cut short, a CRC mismatch or an
// impossible length as a bad frame at that frame's offset, never reading past
// the end of the bytes it was given.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chronolog_errcode.h>
#include <Crc32c.h>
#include <WalFormat.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;
namespace fs = std::filesystem;

namespace
{
std::string le(uint64_t value, int bytes)
{
    std::string out;
    for(int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF)); }
    return out;
}

std::string join(wal::EventFrame const& frame)
{
    return std::string(frame.frame_header.data(), frame.frame_header.size()) + frame.head + std::string(frame.record) +
           frame.tail;
}

// a frame around an arbitrary body, built here rather than by WalFormat
std::string frameOf(std::string const& body)
{
    return le(body.size(), 4) + le(chl::crc32c_extend(0, body.data(), body.size()), 4) + body;
}

wal::HeaderRecord sampleHeader()
{
    return wal::HeaderRecord{wal::kFormatVersion, 42, 1760000000123456789ull, "ofi+sockets://127.0.0.1:5555"};
}

void expectSameEvent(chl::LogEvent const& expected, chl::LogEvent const& actual)
{
    EXPECT_EQ(expected.storyId, actual.storyId);
    EXPECT_EQ(expected.eventTime, actual.eventTime);
    EXPECT_EQ(expected.clientId, actual.clientId);
    EXPECT_EQ(expected.eventIndex, actual.eventIndex);
    EXPECT_EQ(expected.logRecord, actual.logRecord);
}
} // namespace

TEST(WalFormat, EveryRecordTypeRoundTripsThroughScan)
{
    wal::HeaderRecord const header = sampleHeader();
    wal::NamesRecord const names{0x1122334455667788ull, "chronicle-a", "story/with.dots"};
    chl::LogEvent const event(names.story_id, 1760000000000000001ull, 77, 3, "payload bytes");
    std::vector<chl::EventExtension> const extensions = {{9, "nine"}, {0xFFFF, ""}};
    wal::DroppedRecord const dropped{names.story_id};

    std::string const h = wal::encodeHeader(header);
    std::string const n = wal::encodeNames(names);
    std::string const e = join(wal::encodeEvent(event, extensions));
    std::string const d = wal::encodeDropped(dropped);
    std::string segment = h + n + e + d;
    uint64_t const data_end = segment.size();
    segment.append(4096, '\0');

    wal::ScanResult const scan = wal::scanSegment(segment);
    EXPECT_EQ(wal::ScanEnd::EndOfData, scan.end);
    EXPECT_EQ(data_end, scan.end_offset);
    EXPECT_TRUE(scan.reason.empty()) << scan.reason;
    ASSERT_EQ(4u, scan.records.size());

    std::vector<std::pair<char, uint64_t>> const expected = {{'H', 0},
                                                             {'N', h.size()},
                                                             {'E', h.size() + n.size()},
                                                             {'D', h.size() + n.size() + e.size()}};
    for(std::size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(static_cast<uint8_t>(expected[i].first), scan.records[i].type) << i;
        EXPECT_EQ(expected[i].second, scan.records[i].offset) << i;
        EXPECT_EQ(static_cast<char>(scan.records[i].type), scan.records[i].body.at(0)) << "body starts with its type";
    }

    wal::HeaderRecord header_out{};
    ASSERT_EQ(chl::CL_SUCCESS, wal::decodeHeader(scan.records[0].body, header_out));
    EXPECT_EQ(header.format_version, header_out.format_version);
    EXPECT_EQ(header.segment_number, header_out.segment_number);
    EXPECT_EQ(header.created_ns, header_out.created_ns);
    EXPECT_EQ(header.keeper_endpoint, header_out.keeper_endpoint);

    wal::NamesRecord names_out{};
    ASSERT_EQ(chl::CL_SUCCESS, wal::decodeNames(scan.records[1].body, names_out));
    EXPECT_EQ(names.story_id, names_out.story_id);
    EXPECT_EQ(names.chronicle, names_out.chronicle);
    EXPECT_EQ(names.story, names_out.story);

    // an E body is its type byte followed by the LogEventCodec bytes
    std::string_view const e_body = scan.records[2].body;
    chl::LogEvent event_out;
    std::vector<chl::EventExtension> extensions_out;
    ASSERT_EQ(chl::CodecStatus::Ok,
              chl::decodeLogEvent(e_body.data() + 1, e_body.size() - 1, event_out, &extensions_out));
    expectSameEvent(event, event_out);
    ASSERT_EQ(extensions.size(), extensions_out.size());
    for(std::size_t i = 0; i < extensions.size(); ++i)
    {
        EXPECT_EQ(extensions[i].tag, extensions_out[i].tag);
        EXPECT_EQ(extensions[i].value, extensions_out[i].value);
    }

    wal::DroppedRecord dropped_out{};
    ASSERT_EQ(chl::CL_SUCCESS, wal::decodeDropped(scan.records[3].body, dropped_out));
    EXPECT_EQ(dropped.story_id, dropped_out.story_id);
}

TEST(WalFormat, HeaderLayoutIsAsDocumented)
{
    wal::HeaderRecord const header = sampleHeader();
    std::string const body = std::string("H") + "CLWAL" + le(1, 2) + le(header.segment_number, 8) +
                             le(header.created_ns, 8) + le(header.keeper_endpoint.size(), 4) + header.keeper_endpoint;
    EXPECT_EQ(frameOf(body), wal::encodeHeader(header));

    wal::NamesRecord const names{5, "c", "st"};
    EXPECT_EQ(frameOf(std::string("N") + le(5, 8) + le(1, 4) + "c" + le(2, 4) + "st"), wal::encodeNames(names));
    EXPECT_EQ(frameOf(std::string("D") + le(5, 8)), wal::encodeDropped(wal::DroppedRecord{5}));
}

TEST(WalFormat, ZeroLengthAfterRecordsEndsTheData)
{
    std::string const h = wal::encodeHeader(sampleHeader());
    std::string const n = wal::encodeNames(wal::NamesRecord{1, "c", "s"});
    // a zero length ends the data even when a valid frame follows it
    std::string const segment = h + n + std::string(8, '\0') + wal::encodeDropped(wal::DroppedRecord{1});

    wal::ScanResult const scan = wal::scanSegment(segment);
    EXPECT_EQ(wal::ScanEnd::EndOfData, scan.end);
    EXPECT_EQ(h.size() + n.size(), scan.end_offset);
    ASSERT_EQ(2u, scan.records.size());

    // a zero length with stray CRC bytes still ends the data
    std::string const stray = h + le(0, 4) + le(0xDEADBEEF, 4);
    wal::ScanResult const stray_scan = wal::scanSegment(stray);
    EXPECT_EQ(wal::ScanEnd::EndOfData, stray_scan.end);
    EXPECT_EQ(h.size(), stray_scan.end_offset);
}

TEST(WalFormat, DataEndingAtTheSegmentEndIsEndOfData)
{
    std::string const h = wal::encodeHeader(sampleHeader());
    std::string const d = wal::encodeDropped(wal::DroppedRecord{1});

    wal::ScanResult const scan = wal::scanSegment(h + d);
    EXPECT_EQ(wal::ScanEnd::EndOfData, scan.end);
    EXPECT_EQ(h.size() + d.size(), scan.end_offset);
    EXPECT_EQ(2u, scan.records.size());

    // fewer than four bytes left, all zero: no room for a frame, so the data ended
    wal::ScanResult const short_zero = wal::scanSegment(h + std::string(3, '\0'));
    EXPECT_EQ(wal::ScanEnd::EndOfData, short_zero.end);
    EXPECT_EQ(h.size(), short_zero.end_offset);
}

TEST(WalFormat, FrameCutShortIsBadFrameAtItsOffset)
{
    std::string const h = wal::encodeHeader(sampleHeader());
    std::string const n = wal::encodeNames(wal::NamesRecord{1, "chronicle", "story"});
    std::string const e = join(wal::encodeEvent(chl::LogEvent(1, 2, 3, 4, "record")));

    // every cut inside the E frame, from one byte of its length to one byte short
    for(std::size_t keep = 1; keep < e.size(); ++keep)
    {
        std::string const segment = h + n + e.substr(0, keep);
        wal::ScanResult const scan = wal::scanSegment(segment);
        EXPECT_EQ(wal::ScanEnd::BadFrame, scan.end) << "keep " << keep;
        EXPECT_EQ(h.size() + n.size(), scan.end_offset) << "keep " << keep;
        EXPECT_FALSE(scan.reason.empty());
        EXPECT_EQ(2u, scan.records.size()) << "records before the bad frame are returned";
    }
}

TEST(WalFormat, FlippedBodyByteIsBadFrameAtItsOffset)
{
    std::string const h = wal::encodeHeader(sampleHeader());
    std::string const e = join(wal::encodeEvent(chl::LogEvent(1, 2, 3, 4, "record")));
    std::string const d = wal::encodeDropped(wal::DroppedRecord{1});

    // every byte of the E body, type byte included
    for(std::size_t i = 8; i < e.size(); ++i)
    {
        std::string flipped = e;
        flipped[i] = static_cast<char>(flipped[i] ^ 0x01);
        wal::ScanResult const scan = wal::scanSegment(h + flipped + d);
        EXPECT_EQ(wal::ScanEnd::BadFrame, scan.end) << "byte " << i;
        EXPECT_EQ(h.size(), scan.end_offset) << "byte " << i;
        EXPECT_NE(std::string::npos, scan.reason.find("CRC")) << scan.reason;
        EXPECT_EQ(1u, scan.records.size());
    }

    // a flipped CRC byte is a mismatch too
    std::string bad_crc = e;
    bad_crc[5] = static_cast<char>(bad_crc[5] ^ 0x80);
    wal::ScanResult const scan = wal::scanSegment(h + bad_crc + d);
    EXPECT_EQ(wal::ScanEnd::BadFrame, scan.end);
    EXPECT_EQ(h.size(), scan.end_offset);
}

TEST(WalFormat, LengthOver256MiBIsBadFrameAtItsOffset)
{
    std::string const h = wal::encodeHeader(sampleHeader());
    // the length alone is judged: no body follows, and the scan must not look for one
    std::string const segment = h + le(wal::kMaxBodyBytes + 1ull, 4) + le(0, 4) + std::string(64, 'x');

    wal::ScanResult const scan = wal::scanSegment(segment);
    EXPECT_EQ(wal::ScanEnd::BadFrame, scan.end);
    EXPECT_EQ(h.size(), scan.end_offset);
    EXPECT_FALSE(scan.reason.empty());
    EXPECT_EQ(1u, scan.records.size());

    wal::ScanResult const max_length = wal::scanSegment(h + le(0xFFFFFFFFull, 4) + le(0, 4));
    EXPECT_EQ(wal::ScanEnd::BadFrame, max_length.end);
    EXPECT_EQ(h.size(), max_length.end_offset);

    // a length of exactly kMaxBodyBytes is allowed: cut short, the frame is bad
    // because it passes the end, not because of its length
    std::string const at_limit = h + le(wal::kMaxBodyBytes, 4) + le(0, 4) + std::string(64, 'x');
    wal::ScanResult const at_limit_scan = wal::scanSegment(at_limit);
    EXPECT_EQ(wal::ScanEnd::BadFrame, at_limit_scan.end);
    EXPECT_EQ(h.size(), at_limit_scan.end_offset);
    EXPECT_NE(std::string::npos, at_limit_scan.reason.find("passes the end")) << at_limit_scan.reason;
}

TEST(WalFormat, SegmentOfOnlyZerosHasNoRecords)
{
    for(std::size_t size: {0u, 1u, 7u, 8u, 1u << 20})
    {
        wal::ScanResult const scan = wal::scanSegment(std::string(size, '\0'));
        EXPECT_EQ(wal::ScanEnd::EndOfData, scan.end) << size;
        EXPECT_EQ(0u, scan.end_offset) << size;
        EXPECT_TRUE(scan.records.empty()) << size;
    }
}

TEST(WalFormat, EventFrameWrittenAsFourPiecesScansLikeAContiguousCopy)
{
    std::string record(70000, '\0');
    for(std::size_t i = 0; i < record.size(); ++i) { record[i] = static_cast<char>(i * 13 + 5); }
    chl::LogEvent const event(11, 22, 33, 44, record);
    wal::EventFrame const frame = wal::encodeEvent(event, {{1, "one"}});

    EXPECT_EQ(event.logRecord.data(), frame.record.data()) << "the record must not be copied";
    EXPECT_EQ('E', frame.head.at(0));
    std::string const contiguous = join(frame);
    EXPECT_EQ(contiguous.size(), frame.size());
    std::string const body = contiguous.substr(8);
    EXPECT_EQ(le(body.size(), 4) + le(chl::crc32c_extend(0, body.data(), body.size()), 4),
              std::string(frame.frame_header.data(), frame.frame_header.size()));

    // write the four pieces with one pwritev after a header, as the committer does
    std::string pattern = (fs::temp_directory_path() / "wal_format_XXXXXX").string();
    int const fd = mkstemp(pattern.data());
    ASSERT_GE(fd, 0);
    std::string const h = wal::encodeHeader(sampleHeader());
    ASSERT_EQ(static_cast<ssize_t>(h.size()), ::pwrite(fd, h.data(), h.size(), 0));
    iovec iov[4] = {{const_cast<char*>(frame.frame_header.data()), frame.frame_header.size()},
                    {const_cast<char*>(frame.head.data()), frame.head.size()},
                    {const_cast<char*>(frame.record.data()), frame.record.size()},
                    {const_cast<char*>(frame.tail.data()), frame.tail.size()}};
    ASSERT_EQ(static_cast<ssize_t>(frame.size()), ::pwritev(fd, iov, 4, static_cast<off_t>(h.size())));
    ::close(fd);
    std::ifstream in(pattern, std::ios::binary);
    std::string const written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fs::remove(pattern);

    // the scanned bodies point into the bytes scanned, so the copy must outlive the scan
    std::string const copy = h + contiguous;
    EXPECT_EQ(copy, written);
    wal::ScanResult const from_file = wal::scanSegment(written);
    wal::ScanResult const from_copy = wal::scanSegment(copy);
    ASSERT_EQ(2u, from_file.records.size());
    ASSERT_EQ(2u, from_copy.records.size());
    EXPECT_EQ(wal::ScanEnd::EndOfData, from_file.end);
    EXPECT_EQ(from_copy.end_offset, from_file.end_offset);
    EXPECT_EQ(from_copy.records[1].offset, from_file.records[1].offset);
    EXPECT_EQ(from_copy.records[1].body, from_file.records[1].body);

    std::string_view const e_body = from_file.records[1].body;
    chl::LogEvent event_out;
    ASSERT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(e_body.data() + 1, e_body.size() - 1, event_out));
    expectSameEvent(event, event_out);
}

TEST(WalFormat, EventBodyOverTheFrameLimitIsRefused)
{
    // the codec allows an encoded event of exactly kMaxEncodedEventBytes; with
    // the type byte its body would be one byte over kMaxBodyBytes, a frame the
    // scan calls corruption, so it must never be written
    chl::LogEvent event;
    event.storyId = 1;
    event.eventTime = 2;
    event.clientId = 3;
    event.eventIndex = 4;
    std::size_t const codec_overhead = 33 + 2; // head and extension count
    event.logRecord.assign(chl::kMaxEncodedEventBytes - codec_overhead, 'r');
    EXPECT_THROW(wal::encodeEvent(event), std::length_error);

    event.logRecord.resize(event.logRecord.size() - 1);
    wal::EventFrame const largest = wal::encodeEvent(event);
    EXPECT_EQ(8u + wal::kMaxBodyBytes, largest.size());
    EXPECT_EQ(le(wal::kMaxBodyBytes, 4), std::string(largest.frame_header.data(), 4));

    // the largest frame the writer produces must scan back as a whole record,
    // or an event acknowledged as durable would be called corruption by recovery
    std::string const segment = join(largest);
    wal::ScanResult const scan = wal::scanSegment(segment);
    EXPECT_EQ(wal::ScanEnd::EndOfData, scan.end) << scan.reason;
    EXPECT_EQ(8u + wal::kMaxBodyBytes, scan.end_offset);
    ASSERT_EQ(1u, scan.records.size());
    EXPECT_EQ(wal::kMaxBodyBytes, scan.records[0].body.size());

    std::string_view const e_body = scan.records[0].body;
    chl::LogEvent event_out;
    ASSERT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(e_body.data() + 1, e_body.size() - 1, event_out));
    expectSameEvent(event, event_out);
}

TEST(WalFormat, StringRecordBodyOverTheFrameLimitIsRefused)
{
    // an N body is the type byte, the story id and two strings with their
    // lengths: 17 bytes plus the names
    std::size_t const names_overhead = 1 + 8 + 4 + 4;
    wal::NamesRecord names{1, std::string(wal::kMaxBodyBytes + 1 - names_overhead, 'c'), ""};
    EXPECT_THROW(wal::encodeNames(names), std::length_error);

    names.chronicle.resize(names.chronicle.size() - 1);
    std::string const largest = wal::encodeNames(names);
    EXPECT_EQ(8u + wal::kMaxBodyBytes, largest.size());
    EXPECT_EQ(le(wal::kMaxBodyBytes, 4), largest.substr(0, 4));
}

TEST(WalFormat, DecodersRefuseTheWrongTypeAndCutBodies)
{
    std::string const h = wal::encodeHeader(sampleHeader()).substr(8);
    std::string const n = wal::encodeNames(wal::NamesRecord{7, "chronicle", "story"}).substr(8);
    std::string const d = wal::encodeDropped(wal::DroppedRecord{7}).substr(8);

    wal::HeaderRecord header{};
    wal::NamesRecord names{};
    wal::DroppedRecord dropped{};
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(n, header));
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeNames(d, names));
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeDropped(h, dropped));
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(std::string_view(), header));

    for(std::size_t keep = 0; keep < h.size(); ++keep)
    {
        EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(std::string_view(h).substr(0, keep), header)) << keep;
    }
    for(std::size_t keep = 0; keep < n.size(); ++keep)
    {
        EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeNames(std::string_view(n).substr(0, keep), names)) << keep;
    }
    for(std::size_t keep = 0; keep < d.size(); ++keep)
    {
        EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeDropped(std::string_view(d).substr(0, keep), dropped)) << keep;
    }

    // bytes after the last field are not part of a version 1 record
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(h + "x", header));
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeNames(n + "x", names));
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeDropped(d + "x", dropped));

    // a string length past the body
    std::string const long_name = std::string("N") + le(7, 8) + le(1000, 4) + "short" + le(0, 4);
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeNames(long_name, names));
}

TEST(WalFormat, HeaderMagicAndVersionAreChecked)
{
    std::string const h = wal::encodeHeader(sampleHeader()).substr(8);
    wal::HeaderRecord header{};

    std::string bad_magic = h;
    bad_magic[3] = 'X';
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(bad_magic, header));
    EXPECT_EQ(0u, header.format_version) << "no version is reported without the magic";

    // a version this build cannot read is refused, and the version found is
    // reported so recovery can name it; the fields after it are not read
    std::string const version_two = std::string("H") + "CLWAL" + le(2, 2) + "a later layout";
    EXPECT_EQ(chl::CL_ERR_INVALID_ARG, wal::decodeHeader(version_two, header));
    EXPECT_EQ(2u, header.format_version);

    ASSERT_EQ(chl::CL_SUCCESS, wal::decodeHeader(h, header));
    EXPECT_EQ(wal::kFormatVersion, header.format_version);
}

TEST(WalFormat, SegmentFileNames)
{
    EXPECT_EQ("00000000000000000001.wal", wal::segmentFileName(1));
    EXPECT_EQ("00000000010000000000.wal", wal::segmentFileName(10000000000ull));
    EXPECT_EQ("18446744073709551615.wal", wal::segmentFileName(UINT64_MAX));

    for(uint64_t segment: std::vector<uint64_t>{0, 1, 9999999999ull, 10000000000ull, UINT64_MAX})
    {
        EXPECT_EQ(std::optional<uint64_t>(segment), wal::parseSegmentFileName(wal::segmentFileName(segment)))
                << segment;
    }

    for(std::string const& name: {std::string("00000000000000000001.wal.partial"),
                                  std::string("0000000000000000001.wal"),
                                  std::string("000000000000000000001.wal"),
                                  std::string("0000000000000000000a.wal"),
                                  std::string("-0000000000000000001.wal"),
                                  std::string("+0000000000000000001.wal"),
                                  std::string(" 0000000000000000001.wal"),
                                  std::string("00000000000000000001.WAL"),
                                  std::string("00000000000000000001"),
                                  std::string("18446744073709551616.wal"),
                                  std::string("99999999999999999999.wal"),
                                  std::string("lock"),
                                  std::string(".wal"),
                                  std::string()})
    {
        EXPECT_FALSE(wal::parseSegmentFileName(name).has_value()) << "'" << name << "'";
    }
}
