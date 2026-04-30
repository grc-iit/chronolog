#include <gtest/gtest.h>
#include <map>
#include <stdexcept>
#include <string>

#include "chronosql_row_codec.h"

using namespace chronosql;

TEST(ChronoSQLRowCodec, EncodeDecodeRoundtrip)
{
    std::map<std::string, std::string> row{{"id", "42"}, {"name", "alice"}};
    auto payload = encodeRow(row);
    auto decoded = decodeRow(payload);
    EXPECT_EQ(decoded, row);
}

TEST(ChronoSQLRowCodec, EncodeOmitsNothingWhenAllPresent)
{
    std::map<std::string, std::string> row{{"a", "1"}};
    auto payload = encodeRow(row);
    EXPECT_NE(payload.find("\"a\":\"1\""), std::string::npos);
}

TEST(ChronoSQLRowCodec, HandlesEscapedQuotesAndBackslashes)
{
    std::map<std::string, std::string> row{{"k", "she said \"hi\\there\""}};
    auto payload = encodeRow(row);
    auto decoded = decodeRow(payload);
    EXPECT_EQ(decoded["k"], row["k"]);
}

TEST(ChronoSQLRowCodec, DecodeRejectsNonObjectRoot) { EXPECT_THROW(decodeRow("[1,2,3]"), std::runtime_error); }

TEST(ChronoSQLRowCodec, DecodeRejectsInvalidJson) { EXPECT_THROW(decodeRow("not json"), std::runtime_error); }

TEST(ChronoSQLRowCodec, DecodeStringifiesNonStringValues)
{
    auto decoded = decodeRow(R"({"a": 1, "b": true, "c": null})");
    EXPECT_EQ(decoded["a"], "1");
    EXPECT_EQ(decoded["b"], "true");
    EXPECT_EQ(decoded["c"], "null");
}
