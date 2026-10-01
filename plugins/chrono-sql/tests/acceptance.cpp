#include "chronolog/sql/database.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace chronolog;
namespace
{
std::string visor, player, scratch;
client::Client connect()
{
    client::ClientOptions options;
    options.catalog_endpoint = visor;
    options.player_endpoint = player;
    auto c = client::Client::Connect(options);
    if(!c.ok())
        throw std::runtime_error(c.status().ToString());
    return std::move(*c);
}
} // namespace
TEST(SqlStack, ProvenanceAndCompletion)
{
    auto c = connect();
    sql::Database db(c, "sql-tests");
    auto created = db.execute("CREATE TABLE t (n INTEGER,r REAL,s TEXT,b BLOB,ok BOOLEAN)");
    ASSERT_TRUE(created.ok()) << created.status();
    ASSERT_TRUE(db.execute("CREATE TABLE t (n INTEGER,r REAL,s TEXT,b BLOB,ok BOOLEAN)").ok());
    std::vector<sql::Value> parameters = {1,
                                          1.25,
                                          "a",
                                          sql::Value::binary({0, 255}),
                                          true,
                                          2,
                                          2.5,
                                          "b",
                                          sql::Value::binary({1}),
                                          false,
                                          3,
                                          3.75,
                                          "c",
                                          sql::Value::binary({2}),
                                          true};
    auto inserted = db.execute("INSERT INTO t VALUES (?,?,?,?,?),(?,?,?,?,?),(?,?,?,?,?)", parameters);
    ASSERT_TRUE(inserted.ok()) << inserted.status();
    ASSERT_EQ(inserted->receipts.size(), 3);
    for(const auto& r: inserted->receipts)
    {
        ASSERT_TRUE(r.ok());
        EXPECT_TRUE(r->acked());
    }
    for(const auto& [op, want]:
        std::vector<std::pair<std::string, size_t>>{{"=", 1}, {"!=", 2}, {"<", 1}, {">", 1}, {"<=", 2}, {">=", 2}})
    {
        auto rows = db.execute("SELECT n FROM t WHERE n " + op + " 2");
        ASSERT_TRUE(rows.ok()) << rows.status();
        EXPECT_EQ(rows->rows.size(), want);
        ASSERT_TRUE(rows->completion);
        EXPECT_TRUE(rows->completion->complete);
    }
    auto text = db.execute("SELECT s FROM t WHERE s > 'a' AND s <= 'c'");
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(text->rows.size(), 2);
    auto desc = db.execute("SELECT n,_hlc,_event_id,_physical_ns FROM t ORDER BY TIME DESC LIMIT 2");
    ASSERT_TRUE(desc.ok());
    ASSERT_EQ(desc->rows.size(), 2);
    EXPECT_EQ(desc->rows[0]["n"], 3);
    EXPECT_EQ(desc->rows[1]["n"], 2);
    ASSERT_TRUE(desc->completion);
    EXPECT_TRUE(desc->completion->complete);
    auto limit = db.execute("SELECT * FROM t LIMIT 1");
    ASSERT_TRUE(limit.ok());
    EXPECT_TRUE(limit->limited);
    EXPECT_FALSE(limit->completion);
    auto count = db.execute("SELECT COUNT(*) FROM t");
    ASSERT_TRUE(count.ok());
    EXPECT_EQ(count->rows[0]["count"], 3);
    ASSERT_TRUE(count->completion);
    EXPECT_TRUE(count->completion->complete);
    auto lo = (*inserted->receipts[0]).hlc, hi = (*inserted->receipts[2]).hlc;
    auto range = db.execute("SELECT n FROM t WHERE TIME BETWEEN '" + std::to_string(lo.physical_ns) + ":" +
                            std::to_string(lo.logical) + "' AND '" + std::to_string(hi.physical_ns) + ":" +
                            std::to_string(hi.logical) + "'");
    ASSERT_TRUE(range.ok());
    EXPECT_EQ(range->rows.size(), 2);
    std::vector<sql::Value> bounds = {std::to_string(lo.physical_ns) + ":" + std::to_string(lo.logical),
                                      std::to_string(hi.physical_ns) + ":" + std::to_string(hi.logical)};
    auto boundRange = db.execute("SELECT n FROM t WHERE TIME BETWEEN ? AND ?", bounds);
    ASSERT_TRUE(boundRange.ok()) << boundRange.status();
    EXPECT_EQ(boundRange->rows.size(), 2);
    std::vector<sql::Value> physicalBounds = {
            int64_t{0},
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                    1000000000LL};
    auto physical = db.execute("SELECT n FROM t WHERE PHYSICAL BETWEEN ? AND ?", physicalBounds);
    ASSERT_TRUE(physical.ok()) << physical.status();
    EXPECT_EQ(physical->rows.size(), 3);
    ASSERT_TRUE(physical->completion);
    EXPECT_FALSE(physical->completion->complete);
    std::vector<sql::Value> equality = {sql::Value::binary({0, 255})};
    auto blobs = db.execute("SELECT b FROM t WHERE b = ?", equality);
    ASSERT_TRUE(blobs.ok());
    ASSERT_EQ(blobs->rows.size(), 1);
    EXPECT_EQ(blobs->rows[0]["b"], equality[0]);
    for(const auto& query: {"SELECT missing FROM t",
                            "SELECT * FROM t WHERE missing = 1",
                            "SELECT * FROM t WHERE ok < true",
                            "INSERT INTO t (n,n) VALUES (1,2)",
                            "INSERT INTO t VALUES (1)",
                            "SELECT * FROM t WHERE n = 'bad'"})
        EXPECT_TRUE(absl::IsInvalidArgument(db.execute(query).status())) << query;
    auto nulls = db.execute("INSERT INTO t (n) VALUES (4)");
    ASSERT_TRUE(nulls.ok());
    auto omitted = db.execute("SELECT * FROM t WHERE n = 4");
    ASSERT_TRUE(omitted.ok());
    EXPECT_TRUE(omitted->rows[0]["s"].is_null());
    EXPECT_TRUE(absl::IsInvalidArgument(db.execute("INSERT INTO t (n) VALUES ('wrong')").status()));
    auto bad = db.execute("CREATE TABLE t (other TEXT)");
    EXPECT_TRUE(absl::IsAlreadyExists(bad.status()));
    EXPECT_TRUE(absl::IsFailedPrecondition(db.execute("INSERT INTO t (n) VALUES (5)").status()));
    std::ofstream(scratch + "/stop-keeper").put('1');
    for(size_t i = 0; i < 200 && !std::filesystem::exists(scratch + "/keeper-stopped"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ASSERT_TRUE(std::filesystem::exists(scratch + "/keeper-stopped"));
    auto incomplete = db.execute("SELECT COUNT(*) FROM t");
    ASSERT_TRUE(incomplete.ok()) << incomplete.status();
    ASSERT_TRUE(incomplete->completion);
    EXPECT_FALSE(incomplete->completion->complete);
    EXPECT_EQ(incomplete->completion->reason, IncompleteReason::SourceFailed);
}
TEST(SqlStack, ConcurrentCreateConverges)
{
    auto a = connect(), b = connect();
    sql::Database first(a, "sql-race"), second(b, "sql-race");
    absl::Status sa, sb;
    std::thread ta([&] { sa = first.execute("CREATE TABLE t (a INTEGER)").status(); });
    std::thread tb([&] { sb = second.execute("CREATE TABLE t (b TEXT)").status(); });
    ta.join();
    tb.join();
    EXPECT_NE(sa.ok(), sb.ok());
    EXPECT_TRUE(sa.ok() || absl::IsAlreadyExists(sa)) << sa;
    EXPECT_TRUE(sb.ok() || absl::IsAlreadyExists(sb)) << sb;
    auto loser = sa.ok() ? second.execute("INSERT INTO t VALUES ('x')") : first.execute("INSERT INTO t VALUES (1)");
    EXPECT_TRUE(absl::IsFailedPrecondition(loser.status()));
}
int main(int argc, char** argv)
{
    if(argc != 4)
        return 2;
    visor = argv[1];
    player = argv[2];
    scratch = argv[3];
    ::testing::InitGoogleTest(&argc, argv);
    // Run the missing-source case last so the race has a live Keeper.
    ::testing::GTEST_FLAG(filter) = "SqlStack.ConcurrentCreateConverges";
    int race = RUN_ALL_TESTS();
    ::testing::GTEST_FLAG(filter) = "SqlStack.ProvenanceAndCompletion";
    return RUN_ALL_TESTS() || race;
}
