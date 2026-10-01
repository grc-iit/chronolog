#include "chronolog/sql/database.h"
#include <gtest/gtest.h>
using namespace chronolog::sql;
TEST(SqlParser, GrammarAndTypedBindings)
{
    for(const auto& query: {"CREATE TABLE t (a INTEGER,b REAL,c TEXT,d BLOB,e BOOLEAN)",
                            "INSERT INTO t (a,c) VALUES (-1,'O''Brien'),(2,NULL)",
                            "SELECT * FROM t",
                            "SELECT a,_hlc,_physical_ns,_event_id FROM t WHERE a = 1 AND a != 2 AND a < 3 AND a > 0 "
                            "AND a <= 2 AND a >= 1 ORDER BY TIME ASC LIMIT 4",
                            "SELECT * FROM t WHERE PHYSICAL BETWEEN -1 AND 100",
                            "SELECT COUNT(*) FROM t WHERE TIME BETWEEN '1:0' AND '2:1'",
                            "SELECT a FROM t ORDER BY TIME DESC LIMIT 2",
                            "INSERT INTO t VALUES (X'00ff',1.2,true,false,NULL)"})
        EXPECT_TRUE(parse(query).ok()) << query;
    std::vector<Value> parameters = {Value::binary({0, 255}), int64_t{42}, "quote' OR bad"};
    auto bound = parse("INSERT INTO t VALUES (?,?,?)", parameters);
    ASSERT_TRUE(bound.ok());
    EXPECT_EQ(bound->tuples[0], parameters);
    std::vector<Value> range = {"1:2", "3:4"};
    auto timed = parse("SELECT * FROM t WHERE TIME BETWEEN ? AND ?", range);
    ASSERT_TRUE(timed.ok());
    ASSERT_TRUE(timed->range);
    EXPECT_EQ(timed->range->start, (chronolog::Hlc{1, 2}));
    EXPECT_EQ(timed->range->end, (chronolog::Hlc{3, 4}));
}
TEST(SqlParser, RejectionsNameTokens)
{
    for(const auto& query: {"DROP TABLE t",
                            "SELECT * FROM t JOIN u",
                            "SELECT DISTINCT a FROM t",
                            "SELECT * FROM t WHERE a LIKE 'x'",
                            "SELECT * FROM t WHERE a=1 OR b=2",
                            "SELECT * FROM t ORDER BY TIME DESC",
                            "SELECT * FROM t ORDER BY a",
                            "SELECT * FROM t LIMIT -1",
                            "CREATE TABLE t(a UNKNOWN)",
                            "CREATE TABLE t(a TEXT,a TEXT)",
                            "CREATE TABLE t(_hlc TEXT)",
                            "INSERT INTO t VALUES (?)",
                            "INSERT INTO t VALUES ('bad)",
                            "INSERT INTO t VALUES (X'zz')",
                            "SELECT * FROM t WHERE TIME BETWEEN 'x' AND '2:0'",
                            "SELECT * FROM t WHERE PHYSICAL BETWEEN 2 AND 1",
                            "SELECT * FROM t; SELECT * FROM t",
                            "SELECT * FROM t WHERE a ! 1",
                            "SELECT * FROM t WHERE TIME BETWEEN '3:0' AND '2:0'"})
    {
        auto result = parse(query);
        ASSERT_FALSE(result.ok()) << query;
        EXPECT_TRUE(absl::IsInvalidArgument(result.status()));
        EXPECT_NE(result.status().message().find("token"), std::string::npos) << query;
    }
    std::vector<Value> parameters = {1};
    EXPECT_FALSE(parse("SELECT * FROM t", parameters).ok());
}
TEST(SqlCodec, VersionedTypedRoundTrip)
{
    std::vector<Value> values =
            {nullptr, INT64_MIN, INT64_MAX, 1.25, std::string("text\0bytes", 10), true, Value::binary({0, 128, 255})};
    auto decoded = decodeRow(encodeRow(values));
    ASSERT_TRUE(decoded.ok()) << decoded.status();
    EXPECT_EQ(*decoded, values);
    for(const auto& payload:
        {"garbage", "{\"v\":2,\"row\":[]}", "{\"v\":1,\"row\":[{\"blob\":[256]}]}", "{\"v\":1,\"row\":{}}"})
        EXPECT_FALSE(decodeRow(payload).ok());
}
