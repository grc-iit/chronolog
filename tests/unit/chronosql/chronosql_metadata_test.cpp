#include <gtest/gtest.h>

#include "chronosql_metadata.h"

using namespace chronosql;

namespace
{
MetaCreateOp createOp(const std::string& table)
{
    MetaCreateOp op;
    op.table = table;
    op.columns.push_back({"id", ColumnType::INT});
    op.columns.push_back({"name", ColumnType::STRING});
    return op;
}
} // namespace

TEST(ChronoSQLMetadata, RecordCreateThenList)
{
    MetadataState state;
    state.recordCreate(createOp("users"));
    state.recordCreate(createOp("orders"));
    auto tables = state.listTables();
    ASSERT_EQ(tables.size(), 2u);
    EXPECT_EQ(tables[0], "users");
    EXPECT_EQ(tables[1], "orders");
}

TEST(ChronoSQLMetadata, RecordDropRemovesTable)
{
    MetadataState state;
    state.recordCreate(createOp("users"));
    state.recordDrop({"users"});
    EXPECT_FALSE(state.has("users"));
    EXPECT_TRUE(state.listTables().empty());
}

TEST(ChronoSQLMetadata, ApplyEncodedCreateRoundtrip)
{
    MetadataState state;
    auto payload = encodeCreateOp(createOp("users"));
    state.apply(payload);
    ASSERT_TRUE(state.has("users"));
    auto schema = state.get("users");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(schema->columns.size(), 2u);
    EXPECT_EQ(schema->columns[0].name, "id");
    EXPECT_EQ(schema->columns[0].type, ColumnType::INT);
}

TEST(ChronoSQLMetadata, ApplyEncodedDropRoundtrip)
{
    MetadataState state;
    state.apply(encodeCreateOp(createOp("users")));
    state.apply(encodeDropOp({"users"}));
    EXPECT_FALSE(state.has("users"));
}

TEST(ChronoSQLMetadata, ApplyMalformedIsTolerated)
{
    MetadataState state;
    state.apply("not json");
    state.apply("{}");
    state.apply(R"({"op":"unknown_op","name":"x"})");
    EXPECT_TRUE(state.listTables().empty());
}

TEST(ChronoSQLMetadata, RecordCreateIdempotentOnReplay)
{
    MetadataState state;
    state.recordCreate(createOp("users"));
    state.recordCreate(createOp("users")); // duplicate replay
    EXPECT_EQ(state.listTables().size(), 1u);
}
