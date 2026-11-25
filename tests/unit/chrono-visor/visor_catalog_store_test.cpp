#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include <sqlite3.h>

#include <InMemoryCatalogStore.h>
#include <SqliteCatalogStore.h>
#include <chronolog_errcode.h>

namespace fs = std::filesystem;
namespace chl = chronolog;

namespace
{

class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("chronolog_visor_catalog_store_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path const& path() const { return path_; }

private:
    fs::path path_;
};

// Runs one statement on the file through a connection of its own. The store
// must be closed first: it holds an exclusive lock on the file.
void rawExec(fs::path const& file, std::string const& sql)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(file.c_str(), &db), SQLITE_OK);
    char* message = nullptr;
    int const rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    std::string const text = message ? message : "";
    sqlite3_free(message);
    sqlite3_close(db);
    ASSERT_EQ(rc, SQLITE_OK) << text;
}

// A name whose hash has the top bit set, so it is stored as a negative integer.
std::string chronicleNameWithHighBitId()
{
    for(int i = 0;; ++i)
    {
        std::string name = "high_bit_chronicle_" + std::to_string(i);
        if(chl::catalogChronicleIdOf(name) >= (uint64_t{1} << 63))
            return name;
    }
}

std::string storyNameWithHighBitId(std::string const& chronicle)
{
    for(int i = 0;; ++i)
    {
        std::string name = "high_bit_story_" + std::to_string(i);
        if(chl::storyIdOf(chronicle, name) >= (uint64_t{1} << 63))
            return name;
    }
}

int insertChronicle(chl::VisorCatalogStore& store, std::string const& name)
{
    return store.insertChronicle(chl::catalogChronicleIdOf(name), name);
}

int insertStory(chl::VisorCatalogStore& store, std::string const& chronicle, std::string const& story)
{
    return store.insertStory(chl::storyIdOf(chronicle, story), chl::catalogChronicleIdOf(chronicle), chronicle, story);
}

chl::CatalogSnapshot loadOrFail(chl::VisorCatalogStore& store)
{
    chl::CatalogSnapshot snapshot;
    EXPECT_EQ(store.load(snapshot), chl::CL_SUCCESS);
    return snapshot;
}

// A snapshot holding one chronicle and one story, to show that a refused load
// empties it.
chl::CatalogSnapshot filledSnapshot()
{
    chl::CatalogSnapshot snapshot;
    snapshot.chronicles.push_back({chl::catalogChronicleIdOf("stale"), "stale", false});
    snapshot.stories.push_back({chl::storyIdOf("stale", "s"), "stale", "s", false});
    return snapshot;
}

// "chronicle/story*" for each story, '*' marking a pending destroy; chronicles
// as "chronicle*". In the store's id order.
std::vector<std::string> describe(chl::CatalogSnapshot const& snapshot)
{
    std::vector<std::string> out;
    for(auto const& chronicle: snapshot.chronicles)
    {
        EXPECT_EQ(chronicle.id, chl::catalogChronicleIdOf(chronicle.name));
        out.push_back(chronicle.name + (chronicle.destroyPending ? "*" : ""));
    }
    for(auto const& story: snapshot.stories)
    {
        EXPECT_EQ(story.id, chl::storyIdOf(story.chronicle, story.story));
        out.push_back(story.chronicle + "/" + story.story + (story.destroyPending ? "*" : ""));
    }
    return out;
}

std::vector<std::string> sorted(std::vector<std::string> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

struct SqliteStore
{
    static std::unique_ptr<chl::VisorCatalogStore> make(fs::path const& dir)
    {
        return std::make_unique<chl::SqliteCatalogStore>((dir / "catalog.sqlite").string());
    }
    static constexpr bool persistent = true;
};

struct InMemoryStore
{
    static std::unique_ptr<chl::VisorCatalogStore> make(fs::path const&)
    {
        return std::make_unique<chl::InMemoryCatalogStore>();
    }
    static constexpr bool persistent = false;
};

struct StoreNames
{
    template <typename T>
    static std::string GetName(int)
    {
        return std::is_same<T, SqliteStore>::value ? "Sqlite" : "InMemory";
    }
};

} // namespace

template <typename Store>
class CatalogStore: public ::testing::Test
{
protected:
    void SetUp() override
    {
        store = Store::make(dir.path());
        ASSERT_EQ(store->open(), chl::CL_SUCCESS);
    }

    // Closes the store and opens the file again; the in-memory store stays.
    void reopen()
    {
        if(!Store::persistent)
            return;
        store.reset();
        store = Store::make(dir.path());
        ASSERT_EQ(store->open(), chl::CL_SUCCESS);
    }

    TempDir dir;
    std::unique_ptr<chl::VisorCatalogStore> store;
};

using StoreTypes = ::testing::Types<SqliteStore, InMemoryStore>;
TYPED_TEST_SUITE(CatalogStore, StoreTypes, StoreNames);

TYPED_TEST(CatalogStore, ChangesSurviveReopen)
{
    std::string const high = chronicleNameWithHighBitId();
    std::string const highStory = storyNameWithHighBitId("plain");
    ASSERT_EQ(insertChronicle(*this->store, "plain"), chl::CL_SUCCESS);
    ASSERT_EQ(insertChronicle(*this->store, high), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "plain", "kept"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "plain", highStory), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "plain", "deleted"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, high, "under_high"), chl::CL_SUCCESS);
    ASSERT_EQ(this->store->deleteStory(chl::storyIdOf("plain", "deleted")), chl::CL_SUCCESS);

    this->reopen();

    chl::CatalogSnapshot const snapshot = loadOrFail(*this->store);
    EXPECT_EQ(sorted(describe(snapshot)),
              sorted({"plain", high, "plain/kept", "plain/" + highStory, high + "/under_high"}));
    // The high-bit ids come back unsigned, as they went in.
    bool sawHighChronicle = false;
    for(auto const& chronicle: snapshot.chronicles)
        if(chronicle.name == high)
            sawHighChronicle = chronicle.id == chl::catalogChronicleIdOf(high);
    EXPECT_TRUE(sawHighChronicle);
    bool sawHighStory = false;
    for(auto const& story: snapshot.stories)
        if(story.story == highStory)
            sawHighStory = story.id == chl::storyIdOf("plain", highStory);
    EXPECT_TRUE(sawHighStory);
    // Loaded in unsigned id order.
    for(size_t i = 1; i < snapshot.chronicles.size(); ++i)
        EXPECT_LT(snapshot.chronicles[i - 1].id, snapshot.chronicles[i].id);
    for(size_t i = 1; i < snapshot.stories.size(); ++i) EXPECT_LT(snapshot.stories[i - 1].id, snapshot.stories[i].id);
}

TYPED_TEST(CatalogStore, DeleteChronicleTakesItsStories)
{
    ASSERT_EQ(insertChronicle(*this->store, "doomed"), chl::CL_SUCCESS);
    ASSERT_EQ(insertChronicle(*this->store, "other"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "doomed", "a"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "doomed", "b"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "other", "a"), chl::CL_SUCCESS);

    EXPECT_EQ(this->store->deleteChronicle(chl::catalogChronicleIdOf("doomed")), chl::CL_SUCCESS);
    this->reopen();
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"other", "other/a"}));

    EXPECT_EQ(this->store->deleteChronicle(chl::catalogChronicleIdOf("doomed")), chl::CL_ERR_NOT_EXIST);
    EXPECT_EQ(this->store->deleteStory(chl::storyIdOf("doomed", "a")), chl::CL_ERR_NOT_EXIST);
    // The name can be created again, with the same id.
    EXPECT_EQ(insertChronicle(*this->store, "doomed"), chl::CL_SUCCESS);
    EXPECT_EQ(insertStory(*this->store, "doomed", "a"), chl::CL_SUCCESS);
}

TYPED_TEST(CatalogStore, DuplicateInsertIsRefusedAndChangesNothing)
{
    ASSERT_EQ(insertChronicle(*this->store, "c"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "c", "s"), chl::CL_SUCCESS);
    ASSERT_EQ(this->store->setStoryDestroyPending(chl::storyIdOf("c", "s"), true), chl::CL_SUCCESS);
    std::vector<std::string> const before = describe(loadOrFail(*this->store));

    chl::ChronicleId const cid = chl::catalogChronicleIdOf("c");
    EXPECT_EQ(insertChronicle(*this->store, "c"), chl::CL_ERR_CHRONICLE_EXISTS);
    EXPECT_EQ(insertStory(*this->store, "c", "s"), chl::CL_ERR_STORY_EXISTS);
    EXPECT_EQ(insertStory(*this->store, "missing", "s"), chl::CL_ERR_NOT_EXIST);
    // A stored id under another name, or a stored name under another id, is
    // refused for the id before the stored rows are looked at.
    EXPECT_EQ(this->store->insertChronicle(cid, "another_name"), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(this->store->insertChronicle(cid + 1, "c"), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(this->store->insertStory(chl::storyIdOf("c", "s"), cid, "c", "t"), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(this->store->insertStory(chl::storyIdOf("c", "s") + 1, cid, "c", "s"), chl::CL_ERR_INVALID_ARG);

    this->reopen();
    EXPECT_EQ(describe(loadOrFail(*this->store)), before);
}

TYPED_TEST(CatalogStore, DestroyPendingFlagSurvivesReopenAndGoesWithTheRow)
{
    ASSERT_EQ(insertChronicle(*this->store, "c"), chl::CL_SUCCESS);
    ASSERT_EQ(insertChronicle(*this->store, "d"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "c", "s"), chl::CL_SUCCESS);
    ASSERT_EQ(insertStory(*this->store, "c", "t"), chl::CL_SUCCESS);
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"c", "d", "c/s", "c/t"}));

    EXPECT_EQ(this->store->setStoryDestroyPending(chl::storyIdOf("c", "s"), true), chl::CL_SUCCESS);
    EXPECT_EQ(this->store->setChronicleDestroyPending(chl::catalogChronicleIdOf("d"), true), chl::CL_SUCCESS);
    this->reopen();
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"c", "d*", "c/s*", "c/t"}));

    // Cleared explicitly.
    EXPECT_EQ(this->store->setChronicleDestroyPending(chl::catalogChronicleIdOf("d"), false), chl::CL_SUCCESS);
    // Cleared with the row: a story created again starts without the flag.
    EXPECT_EQ(this->store->deleteStory(chl::storyIdOf("c", "s")), chl::CL_SUCCESS);
    EXPECT_EQ(insertStory(*this->store, "c", "s"), chl::CL_SUCCESS);
    this->reopen();
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"c", "d", "c/s", "c/t"}));

    // A chronicle's flag goes with it too.
    EXPECT_EQ(this->store->setChronicleDestroyPending(chl::catalogChronicleIdOf("c"), true), chl::CL_SUCCESS);
    EXPECT_EQ(this->store->deleteChronicle(chl::catalogChronicleIdOf("c")), chl::CL_SUCCESS);
    EXPECT_EQ(insertChronicle(*this->store, "c"), chl::CL_SUCCESS);
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"c", "d"}));

    EXPECT_EQ(this->store->setStoryDestroyPending(chl::storyIdOf("c", "gone"), true), chl::CL_ERR_NOT_EXIST);
    EXPECT_EQ(this->store->setChronicleDestroyPending(chl::catalogChronicleIdOf("gone"), true), chl::CL_ERR_NOT_EXIST);
}

TYPED_TEST(CatalogStore, InsertRefusesAnIdThatDoesNotMatchItsNames)
{
    chl::ChronicleId const cid = chl::catalogChronicleIdOf("c");
    chl::StoryId const sid = chl::storyIdOf("c", "s");
    EXPECT_EQ(this->store->insertChronicle(cid ^ 1, "c"), chl::CL_ERR_INVALID_ARG);
    ASSERT_EQ(insertChronicle(*this->store, "c"), chl::CL_SUCCESS);
    ASSERT_EQ(insertChronicle(*this->store, "d"), chl::CL_SUCCESS);
    // A story id that is not storyIdOf its names.
    EXPECT_EQ(this->store->insertStory(sid ^ 1, cid, "c", "s"), chl::CL_ERR_INVALID_ARG);
    // A chronicle id that is not the hash of the chronicle's name, also when
    // it is the id of another stored chronicle.
    EXPECT_EQ(this->store->insertStory(sid, chl::catalogChronicleIdOf("d"), "c", "s"), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(this->store->insertStory(sid, cid ^ 1, "c", "s"), chl::CL_ERR_INVALID_ARG);

    this->reopen();
    EXPECT_EQ(sorted(describe(loadOrFail(*this->store))), sorted({"c", "d"}));
    EXPECT_EQ(insertStory(*this->store, "c", "s"), chl::CL_SUCCESS);
}

TEST(SqliteCatalogStore, JournalModeIsWalAndSynchronousIsFull)
{
    TempDir dir;
    chl::SqliteCatalogStore store((dir.path() / "catalog.sqlite").string());
    ASSERT_EQ(store.open(), chl::CL_SUCCESS);
    EXPECT_EQ(store.pragmaValue("journal_mode"), "wal");
    EXPECT_EQ(store.pragmaValue("synchronous"), "2"); // FULL
    EXPECT_EQ(store.pragmaValue("foreign_keys"), "1");
    EXPECT_EQ(store.pragmaValue("locking_mode"), "exclusive");
    EXPECT_EQ(store.pragmaValue("busy_timeout"), "5000");
}

TEST(SqliteCatalogStore, SecondOpenOfTheSameFileFails)
{
    TempDir dir;
    std::string const path = (dir.path() / "catalog.sqlite").string();
    // The second open waits this long for the lock before it fails.
    int const busyTimeoutMs = 100;
    {
        chl::SqliteCatalogStore first(path, busyTimeoutMs);
        ASSERT_EQ(first.open(), chl::CL_SUCCESS);
        ASSERT_EQ(insertChronicle(first, "c"), chl::CL_SUCCESS);
    }
    // An existing file, opened with no write to make: the lock must still be held.
    chl::SqliteCatalogStore first(path, busyTimeoutMs);
    ASSERT_EQ(first.open(), chl::CL_SUCCESS);
    EXPECT_EQ(first.pragmaValue("busy_timeout"), std::to_string(busyTimeoutMs));

    chl::SqliteCatalogStore second(path, busyTimeoutMs);
    EXPECT_EQ(second.open(), chl::CL_ERR_INVALID_CONF);
    EXPECT_EQ(insertChronicle(second, "d"), chl::CL_ERR_UNKNOWN);

    // The first store is unaffected.
    EXPECT_EQ(insertChronicle(first, "e"), chl::CL_SUCCESS);
    EXPECT_EQ(sorted(describe(loadOrFail(first))), sorted({"c", "e"}));
}

TEST(SqliteCatalogStore, NewerSchemaVersionIsRefused)
{
    TempDir dir;
    fs::path const file = dir.path() / "catalog.sqlite";
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        ASSERT_EQ(insertChronicle(store, "c"), chl::CL_SUCCESS);
    }
    rawExec(file, "UPDATE schema_version SET version = " + std::to_string(chl::SqliteCatalogStore::SCHEMA_VERSION + 1));
    {
        chl::SqliteCatalogStore store(file.string());
        EXPECT_EQ(store.open(), chl::CL_ERR_INVALID_CONF);
        chl::CatalogSnapshot snapshot;
        EXPECT_NE(store.load(snapshot), chl::CL_SUCCESS);
    }
    // The refusal changed nothing: the older version opens again once restored.
    rawExec(file, "UPDATE schema_version SET version = " + std::to_string(chl::SqliteCatalogStore::SCHEMA_VERSION));
    chl::SqliteCatalogStore store(file.string());
    ASSERT_EQ(store.open(), chl::CL_SUCCESS);
    EXPECT_EQ(describe(loadOrFail(store)), std::vector<std::string>{"c"});
}

TEST(SqliteCatalogStore, MismatchedIdIsRefused)
{
    TempDir dir;
    fs::path const file = dir.path() / "catalog.sqlite";
    chl::ChronicleId const cid = chl::catalogChronicleIdOf("c");
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        ASSERT_EQ(insertChronicle(store, "c"), chl::CL_SUCCESS);
        ASSERT_EQ(insertStory(store, "c", "s"), chl::CL_SUCCESS);
    }

    // A story row whose id is not storyIdOf its names.
    rawExec(file,
            "INSERT INTO stories(id, chronicle_id, name) VALUES (12345, " + std::to_string(static_cast<int64_t>(cid)) +
                    ", 'wrong')");
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        // A refused load empties out, also when it held rows before.
        chl::CatalogSnapshot snapshot = filledSnapshot();
        EXPECT_EQ(store.load(snapshot), chl::CL_ERR_INVALID_CONF);
        EXPECT_TRUE(snapshot.chronicles.empty());
        EXPECT_TRUE(snapshot.stories.empty());
    }

    // A chronicle row whose id is not the hash of its name.
    rawExec(file, "DELETE FROM stories WHERE id = 12345");
    rawExec(file, "INSERT INTO chronicles(id, name) VALUES (-7, 'wrong')");
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        chl::CatalogSnapshot snapshot = filledSnapshot();
        EXPECT_EQ(store.load(snapshot), chl::CL_ERR_INVALID_CONF);
        EXPECT_TRUE(snapshot.chronicles.empty());
        EXPECT_TRUE(snapshot.stories.empty());
    }

    rawExec(file, "DELETE FROM chronicles WHERE id = -7");
    chl::SqliteCatalogStore store(file.string());
    ASSERT_EQ(store.open(), chl::CL_SUCCESS);
    EXPECT_EQ(sorted(describe(loadOrFail(store))), sorted({"c", "c/s"}));
}

TEST(SqliteCatalogStore, StoryOfAMissingChronicleIsRefused)
{
    TempDir dir;
    fs::path const file = dir.path() / "catalog.sqlite";
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        ASSERT_EQ(insertChronicle(store, "c"), chl::CL_SUCCESS);
        ASSERT_EQ(insertStory(store, "c", "s"), chl::CL_SUCCESS);
    }

    // A story row, with a matching id, whose chronicle row is missing. Only a
    // connection with foreign keys off can write it.
    chl::ChronicleId const orphanCid = chl::catalogChronicleIdOf("orphan_chronicle");
    rawExec(file,
            "PRAGMA foreign_keys=OFF; INSERT INTO stories(id, chronicle_id, name) VALUES (" +
                    std::to_string(static_cast<int64_t>(chl::storyIdOf("orphan_chronicle", "s"))) + ", " +
                    std::to_string(static_cast<int64_t>(orphanCid)) + ", 's')");
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        chl::CatalogSnapshot snapshot = filledSnapshot();
        EXPECT_EQ(store.load(snapshot), chl::CL_ERR_INVALID_CONF);
        EXPECT_TRUE(snapshot.chronicles.empty());
        EXPECT_TRUE(snapshot.stories.empty());
    }

    rawExec(file, "DELETE FROM stories WHERE chronicle_id = " + std::to_string(static_cast<int64_t>(orphanCid)));
    chl::SqliteCatalogStore store(file.string());
    ASSERT_EQ(store.open(), chl::CL_SUCCESS);
    EXPECT_EQ(sorted(describe(loadOrFail(store))), sorted({"c", "c/s"}));
}

TEST(SqliteCatalogStore, MissingDirectoryIsCreated)
{
    TempDir dir;
    fs::path const file = dir.path() / "visor" / "nested" / "catalog.sqlite";
    ASSERT_FALSE(fs::exists(file.parent_path()));
    {
        chl::SqliteCatalogStore store(file.string());
        ASSERT_EQ(store.open(), chl::CL_SUCCESS);
        ASSERT_EQ(insertChronicle(store, "c"), chl::CL_SUCCESS);
    }
    EXPECT_TRUE(fs::is_regular_file(file));
    chl::SqliteCatalogStore store(file.string());
    ASSERT_EQ(store.open(), chl::CL_SUCCESS);
    EXPECT_EQ(describe(loadOrFail(store)), std::vector<std::string>{"c"});
}

TEST(SqliteCatalogStore, UnwritableDirectoryIsRefused)
{
    TempDir dir;
    fs::path const blocker = dir.path() / "not_a_directory";
    {
        std::FILE* f = std::fopen(blocker.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fclose(f);
    }
    chl::SqliteCatalogStore store((blocker / "catalog.sqlite").string());
    EXPECT_EQ(store.open(), chl::CL_ERR_INVALID_CONF);
    EXPECT_EQ(insertChronicle(store, "c"), chl::CL_ERR_UNKNOWN);
}
