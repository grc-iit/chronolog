// Unit tests for LogEventCodec. The keeper WAL stores every durable event in
// this encoding, and recovery decodes it after a crash, so the layout must be
// exact, a record cut short anywhere must be reported as truncated rather than
// read past its end, and a version this build does not know must be refused.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <LogEventCodec.h>

namespace chl = chronolog;

namespace
{
std::string join(chl::EncodedEventParts const& parts) { return parts.head + std::string(parts.record) + parts.tail; }

std::string le(uint64_t value, int bytes)
{
    std::string out;
    for(int i = 0; i < bytes; ++i) { out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF)); }
    return out;
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

TEST(LogEventCodec, RoundTrip)
{
    std::string mib(1u << 20, '\0');
    for(std::size_t i = 0; i < mib.size(); ++i) { mib[i] = static_cast<char>(i * 31 + 7); }

    std::vector<chl::LogEvent> const events = {
            chl::LogEvent(1, 2, 3, 4, ""),
            chl::LogEvent(0x0102030405060708ull, 0x1112131415161718ull, 0x2122232425262728ull, 0x31323334u, "abc"),
            chl::LogEvent(std::numeric_limits<uint64_t>::max(),
                          std::numeric_limits<uint64_t>::max(),
                          std::numeric_limits<uint64_t>::max(),
                          std::numeric_limits<uint32_t>::max(),
                          mib)};

    for(chl::LogEvent const& event: events)
    {
        chl::EncodedEventParts const parts = chl::encodeLogEvent(event);
        EXPECT_EQ(33u, parts.head.size());
        EXPECT_EQ(event.logRecord.data(), parts.record.data()) << "the record must not be copied";
        EXPECT_EQ(event.logRecord.size(), parts.record.size());
        EXPECT_EQ(std::string(2, '\0'), parts.tail);

        std::string const bytes = join(parts);
        chl::LogEvent decoded;
        std::vector<chl::EventExtension> extensions = {{9, "stale"}};
        ASSERT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(bytes.data(), bytes.size(), decoded, &extensions));
        expectSameEvent(event, decoded);
        EXPECT_TRUE(extensions.empty());
    }

    // the layout byte for byte, little-endian
    chl::LogEvent const event(0x0102030405060708ull, 0x1112131415161718ull, 0x2122232425262728ull, 0x31323334u, "abc");
    std::string const expected = std::string(1, '\x01') + le(0x0102030405060708ull, 8) + le(0x1112131415161718ull, 8) +
                                 le(0x2122232425262728ull, 8) + le(0x31323334u, 4) + le(3, 4) + "abc" + le(0, 2);
    EXPECT_EQ(expected, join(chl::encodeLogEvent(event)));
}

TEST(LogEventCodec, ExtensionsRoundTripAndUnknownTagsAreReturned)
{
    chl::LogEvent const event(7, 8, 9, 10, "payload");
    std::vector<chl::EventExtension> const extensions = {{0, ""},
                                                         {1, "one"},
                                                         {0xBEEF, std::string("\0\x01\x02", 3)},
                                                         {0xFFFF, std::string(70000, 'x')}};

    chl::EncodedEventParts const parts = chl::encodeLogEvent(event, extensions);
    std::string const expected_tail = le(4, 2) + le(0, 2) + le(0, 4) + le(1, 2) + le(3, 4) + "one" + le(0xBEEF, 2) +
                                      le(3, 4) + std::string("\0\x01\x02", 3) + le(0xFFFF, 2) + le(70000, 4) +
                                      std::string(70000, 'x');
    EXPECT_EQ(expected_tail, parts.tail);

    std::string const bytes = join(parts);
    chl::LogEvent decoded;
    std::vector<chl::EventExtension> decoded_extensions;
    ASSERT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(bytes.data(), bytes.size(), decoded, &decoded_extensions));
    expectSameEvent(event, decoded);
    ASSERT_EQ(extensions.size(), decoded_extensions.size());
    for(std::size_t i = 0; i < extensions.size(); ++i)
    {
        EXPECT_EQ(extensions[i].tag, decoded_extensions[i].tag) << "extension " << i;
        EXPECT_EQ(extensions[i].value, decoded_extensions[i].value) << "extension " << i;
    }

    // a caller that wants no extensions still gets the event
    chl::LogEvent without;
    ASSERT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(bytes.data(), bytes.size(), without));
    expectSameEvent(event, without);
}

TEST(LogEventCodec, TruncatedInputAtEveryLengthIsTruncated)
{
    chl::LogEvent const event(11, 12, 13, 14, std::string(100, 'r'));
    std::string const bytes = join(chl::encodeLogEvent(event, {{1, "first"}, {2, ""}, {3, "third"}}));

    for(std::size_t len = 0; len < bytes.size(); ++len)
    {
        chl::LogEvent decoded(99, 99, 99, 99, "untouched");
        std::vector<chl::EventExtension> extensions;
        ASSERT_EQ(chl::CodecStatus::Truncated, chl::decodeLogEvent(bytes.data(), len, decoded, &extensions))
                << "length " << len << " of " << bytes.size();
        EXPECT_EQ("untouched", decoded.logRecord) << "length " << len;
        EXPECT_TRUE(extensions.empty()) << "length " << len;
    }

    chl::LogEvent decoded;
    EXPECT_EQ(chl::CodecStatus::Ok, chl::decodeLogEvent(bytes.data(), bytes.size(), decoded));
    expectSameEvent(event, decoded);
}

TEST(LogEventCodec, RecordLengthOver256MiBIsTooLarge)
{
    chl::LogEvent const event(1, 2, 3, 4, "abc");
    std::string bytes = join(chl::encodeLogEvent(event, {{5, "ext"}}));

    // only the declared length is needed to refuse it: the record is not there
    std::string const head = bytes.substr(0, 29) + le(chl::kMaxEncodedEventBytes + 1, 4);
    chl::LogEvent decoded;
    EXPECT_EQ(chl::CodecStatus::TooLarge, chl::decodeLogEvent(head.data(), head.size(), decoded));
    std::string const huge = bytes.substr(0, 29) + le(0xFFFFFFFFu, 4);
    EXPECT_EQ(chl::CodecStatus::TooLarge, chl::decodeLogEvent(huge.data(), huge.size(), decoded));

    // an extension length is held to the same limit
    std::size_t const ext_length_at = 33 + 3 + 2 + 2;
    std::string ext = bytes;
    ext.replace(ext_length_at, 4, le(chl::kMaxEncodedEventBytes, 4));
    EXPECT_EQ(chl::CodecStatus::TooLarge, chl::decodeLogEvent(ext.data(), ext.size(), decoded));

    // and the encoder refuses to produce what the decoder would refuse
    std::string const over(chl::kMaxEncodedEventBytes, 'x');
    chl::LogEvent const big(1, 2, 3, 4, over);
    EXPECT_THROW(chl::encodeLogEvent(big), std::length_error);
}

TEST(LogEventCodec, VersionTwoIsUnsupported)
{
    chl::LogEvent const event(1, 2, 3, 4, "abc");
    std::string bytes = join(chl::encodeLogEvent(event));
    ASSERT_EQ(static_cast<char>(chl::kLogEventCodecVersion), bytes[0]);

    chl::LogEvent decoded;
    for(int version: {0, 2, 255})
    {
        bytes[0] = static_cast<char>(version);
        EXPECT_EQ(chl::CodecStatus::UnsupportedVersion, chl::decodeLogEvent(bytes.data(), bytes.size(), decoded))
                << "version " << version;
        // the version is judged before anything after it
        EXPECT_EQ(chl::CodecStatus::UnsupportedVersion, chl::decodeLogEvent(bytes.data(), 1, decoded))
                << "version " << version;
    }
}
