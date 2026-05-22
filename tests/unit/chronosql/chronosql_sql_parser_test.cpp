#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <variant>

#include "chronosql_sql_parser.h"

using namespace chronosql;

TEST(ChronoSQLParser, ParsesCreateTableWithTypes)
{
    auto stmt = parseStatement("CREATE TABLE users (id INT, name STRING, weight DOUBLE)");
    auto* p = std::get_if<ParsedCreateTable>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->table, "users");
    ASSERT_EQ(p->columns.size(), 3u);
    EXPECT_EQ(p->columns[0].name, "id");
    EXPECT_EQ(p->columns[0].type, ColumnType::INT);
    EXPECT_EQ(p->columns[1].type, ColumnType::STRING);
    EXPECT_EQ(p->columns[2].type, ColumnType::DOUBLE);
}

TEST(ChronoSQLParser, ParsesCreateTableUntyped)
{
    auto stmt = parseStatement("CREATE TABLE t (a, b, c)");
    auto* p = std::get_if<ParsedCreateTable>(&stmt);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->columns.size(), 3u);
    EXPECT_EQ(p->columns[0].type, ColumnType::STRING); // default
}

TEST(ChronoSQLParser, ParsesCreateTableVarcharLength)
{
    auto stmt = parseStatement("CREATE TABLE t (a VARCHAR(255), b TEXT)");
    auto* p = std::get_if<ParsedCreateTable>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->columns[0].type, ColumnType::STRING);
    EXPECT_EQ(p->columns[1].type, ColumnType::STRING);
}

TEST(ChronoSQLParser, ParsesDropTable)
{
    auto stmt = parseStatement("DROP TABLE users;");
    auto* p = std::get_if<ParsedDropTable>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->table, "users");
}

TEST(ChronoSQLParser, ParsesInsertWithColumns)
{
    auto stmt = parseStatement("INSERT INTO users (id, name) VALUES (42, 'alice')");
    auto* p = std::get_if<ParsedInsert>(&stmt);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->columns.size(), 2u);
    EXPECT_EQ(p->columns[0], "id");
    EXPECT_EQ(p->values[0], "42");
    EXPECT_EQ(p->values[1], "alice");
    EXPECT_FALSE(p->is_null[0]);
}

TEST(ChronoSQLParser, ParsesInsertWithoutColumns)
{
    auto stmt = parseStatement("INSERT INTO users VALUES (1, 'bob', NULL)");
    auto* p = std::get_if<ParsedInsert>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->columns.empty());
    ASSERT_EQ(p->values.size(), 3u);
    EXPECT_TRUE(p->is_null[2]);
}

TEST(ChronoSQLParser, ParsesInsertEscapedQuote)
{
    auto stmt = parseStatement("INSERT INTO t VALUES ('it''s fine')");
    auto* p = std::get_if<ParsedInsert>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->values[0], "it's fine");
}

TEST(ChronoSQLParser, ParsesSelectStar)
{
    auto stmt = parseStatement("SELECT * FROM users");
    auto* p = std::get_if<ParsedSelect>(&stmt);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->projection.empty());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(p->where));
}

TEST(ChronoSQLParser, ParsesSelectColumns)
{
    auto stmt = parseStatement("SELECT id, name FROM users");
    auto* p = std::get_if<ParsedSelect>(&stmt);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->projection.size(), 2u);
    EXPECT_EQ(p->projection[0], "id");
}

TEST(ChronoSQLParser, ParsesSelectWhereEq)
{
    auto stmt = parseStatement("SELECT * FROM users WHERE id = 42");
    auto* p = std::get_if<ParsedSelect>(&stmt);
    ASSERT_NE(p, nullptr);
    auto* w = std::get_if<WhereEq>(&p->where);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->column, "id");
    EXPECT_EQ(w->value, "42");
}

TEST(ChronoSQLParser, ParsesSelectWhereTsBetween)
{
    auto stmt = parseStatement("SELECT * FROM users WHERE __ts BETWEEN 100 AND 200");
    auto* p = std::get_if<ParsedSelect>(&stmt);
    ASSERT_NE(p, nullptr);
    auto* w = std::get_if<WhereTsBetween>(&p->where);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->lo, 100u);
    EXPECT_EQ(w->hi, 200u);
}

TEST(ChronoSQLParser, RejectsUnknownStatement)
{
    EXPECT_THROW(parseStatement("UPDATE users SET name='x'"), std::invalid_argument);
}

TEST(ChronoSQLParser, RejectsTrailingTokens)
{
    EXPECT_THROW(parseStatement("SELECT * FROM users GARBAGE"), std::invalid_argument);
}

TEST(ChronoSQLParser, RejectsMissingValues)
{
    EXPECT_THROW(parseStatement("INSERT INTO t (a, b)"), std::invalid_argument);
}

TEST(ChronoSQLParser, RejectsUnknownType)
{
    EXPECT_THROW(parseStatement("CREATE TABLE t (a FOOBAR)"), std::invalid_argument);
}

TEST(ChronoSQLParser, IdentifiersAreCaseInsensitive)
{
    // SQL standard: unquoted identifiers are case-insensitive. The parser normalizes table and
    // column names to lower-case so the metadata store can do exact-string lookups without
    // surprising the user.
    auto createStmt = parseStatement("CREATE TABLE Users (Id INT, Name STRING)");
    auto* c = std::get_if<ParsedCreateTable>(&createStmt);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->table, "users");
    ASSERT_EQ(c->columns.size(), 2u);
    EXPECT_EQ(c->columns[0].name, "id");
    EXPECT_EQ(c->columns[1].name, "name");

    auto insertStmt = parseStatement("INSERT INTO Users (Id, Name) VALUES (1, 'a')");
    auto* i = std::get_if<ParsedInsert>(&insertStmt);
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(i->table, "users");
    ASSERT_EQ(i->columns.size(), 2u);
    EXPECT_EQ(i->columns[0], "id");
    EXPECT_EQ(i->columns[1], "name");

    auto selectStmt = parseStatement("SELECT Id, Name FROM Users WHERE Id = 1");
    auto* s = std::get_if<ParsedSelect>(&selectStmt);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->table, "users");
    ASSERT_EQ(s->projection.size(), 2u);
    EXPECT_EQ(s->projection[0], "id");
    EXPECT_EQ(s->projection[1], "name");
    auto* w = std::get_if<WhereEq>(&s->where);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->column, "id");
}
