#include <gtest/gtest.h>

#include <sqlite3.h>

#include "TestSupport.h"
#include "catalog/SqliteMetadataStore.h"

namespace chronolog::visor
{
namespace
{

using testing::TempDir;
using testing::twoKeeperTopology;

std::unique_ptr<SqliteMetadataStore> openStore(const TempDir& dir)
{
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(), twoKeeperTopology());
    EXPECT_TRUE(store.ok()) << store.status();
    return store.ok() ? std::move(*store) : nullptr;
}

TEST(sqlite_restart, ThirdAcquireAfterReopenGetsIncarnationThreeAndTheSameWriterId)
{
    TempDir dir;
    StoryId story = 0;
    uint64_t writer_id = 0;
    {
        auto store = openStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->createChronicle("c").ok());
        auto created = store->createStory("c", "s");
        ASSERT_TRUE(created.ok());
        story = created->id;

        auto first = store->acquire(story, "w1");
        ASSERT_TRUE(first.ok());
        EXPECT_EQ(first->incarnation, 1u);
        writer_id = first->writer_id;
        ASSERT_TRUE(store->release(story, first->writer_id, first->incarnation).ok());

        auto second = store->acquire(story, "w1");
        ASSERT_TRUE(second.ok());
        EXPECT_EQ(second->incarnation, 2u);
        EXPECT_EQ(second->writer_id, writer_id);
    }
    // The second incarnation was never released, as after a writer crash. The store
    // is closed and reopened, and the next acquire supersedes it.
    auto reopened = openStore(dir);
    ASSERT_NE(reopened, nullptr);
    auto third = reopened->acquire(story, "w1");
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(third->incarnation, 3u);
    EXPECT_EQ(third->writer_id, writer_id);
}

TEST(sqlite_restart, IdsAndRevisionsKeepIncreasingAcrossReopen)
{
    TempDir dir;
    uint64_t revision_before = 0;
    StoryId first_story = 0;
    {
        auto store = openStore(dir);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->createChronicle("c").ok());
        first_story = store->createStory("c", "s1")->id;
        ASSERT_TRUE(store->acquire(first_story, "w").ok());
        revision_before = store->snapshotAcquisitions()->revision;
        EXPECT_GT(revision_before, 0u);
    }
    auto store = openStore(dir);
    ASSERT_NE(store, nullptr);
    auto snapshot = store->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot->revision, revision_before);
    ASSERT_EQ(snapshot->active.size(), 1u);
    EXPECT_EQ(snapshot->active[0].story_id, first_story);
    EXPECT_EQ(store->createStory("c", "s2")->id, first_story + 1);
    auto acquired = store->acquire(first_story, "w");
    ASSERT_TRUE(acquired.ok());
    EXPECT_GT(store->snapshotAcquisitions()->revision, revision_before);
}

TEST(sqlite_restart, JournalModeIsWalAndSynchronousIsFull)
{
    TempDir dir;
    auto store = openStore(dir);
    ASSERT_NE(store, nullptr);
    auto mode = store->pragmaValue("journal_mode");
    ASSERT_TRUE(mode.ok());
    EXPECT_EQ(*mode, "wal");
    auto sync = store->pragmaValue("synchronous");
    ASSERT_TRUE(sync.ok());
    EXPECT_EQ(*sync, "2");
    auto fk = store->pragmaValue("foreign_keys");
    ASSERT_TRUE(fk.ok());
    EXPECT_EQ(*fk, "1");

    // The mode is stored in the file, so a fresh connection sees it as well.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2((dir.path() / "catalog.sqlite").c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(raw, "PRAGMA journal_mode", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "wal");
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
}

TEST(sqlite_restart, UnsupportedSchemaVersionIsRefused)
{
    TempDir dir;
    {
        auto store = openStore(dir);
        ASSERT_NE(store, nullptr);
    }
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open((dir.path() / "catalog.sqlite").c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE schema_version SET version = 99", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(), twoKeeperTopology());
    EXPECT_EQ(store.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(sqlite_restart, LiveStoryNamesAreUniquePerChronicle)
{
    TempDir dir;
    auto store = openStore(dir);
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(store->createChronicle("c1").ok());
    ASSERT_TRUE(store->createChronicle("c2").ok());
    ASSERT_TRUE(store->createStory("c1", "same").ok());
    EXPECT_EQ(store->createStory("c1", "same").status().code(), absl::StatusCode::kAlreadyExists);
    EXPECT_TRUE(store->createStory("c2", "same").ok());
}

} // namespace
} // namespace chronolog::visor
