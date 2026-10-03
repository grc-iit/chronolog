#include "catalog/SqliteMetadataStore.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#include "absl/strings/str_cat.h"
#include "adapter/Convert.h"

namespace chronolog::visor
{

namespace
{

constexpr int kSchemaVersion = 2;
constexpr Epoch kInitialEpoch = 1;
constexpr int kBusyTimeoutMs = 5000;

absl::Status sqliteError(sqlite3* db, const char* what)
{
    return absl::UnavailableError(absl::StrCat("sqlite ", what, ": ", sqlite3_errmsg(db)));
}

class Statement
{
public:
    Statement(sqlite3* db, const char* sql)
        : db_(db)
    {
        rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    absl::Status prepared() const { return rc_ == SQLITE_OK ? absl::OkStatus() : sqliteError(db_, "prepare"); }

    Statement& text(int index, const std::string& value)
    {
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& integer(int index, uint64_t value)
    {
        sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
        return *this;
    }
    // Returns true on a row, false on done, and an error otherwise.
    absl::StatusOr<bool> step()
    {
        const int rc = sqlite3_step(stmt_);
        if(rc == SQLITE_ROW)
            return true;
        if(rc == SQLITE_DONE)
            return false;
        return sqliteError(db_, "step");
    }
    uint64_t column(int index) const { return static_cast<uint64_t>(sqlite3_column_int64(stmt_, index)); }
    std::string columnText(int index) const
    {
        const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
        return raw ? std::string(raw, static_cast<size_t>(sqlite3_column_bytes(stmt_, index))) : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_{};
    int rc_{};
};

absl::Status exec(sqlite3* db, const char* sql)
{
    char* message = nullptr;
    if(sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        std::string text = message ? message : "unknown error";
        sqlite3_free(message);
        return absl::UnavailableError(absl::StrCat("sqlite exec: ", text));
    }
    return absl::OkStatus();
}

// BEGIN IMMEDIATE ... COMMIT. Rolls back unless commit() succeeded.
class Transaction
{
public:
    explicit Transaction(sqlite3* db)
        : db_(db)
    {
        nested_ = !sqlite3_get_autocommit(db_);
        status_ = exec(db_, nested_ ? "SAVEPOINT mutation" : "BEGIN IMMEDIATE");
    }
    ~Transaction()
    {
        if(status_.ok() && !committed_)
            sqlite3_exec(db_,
                         nested_ ? "ROLLBACK TO mutation; RELEASE mutation" : "ROLLBACK",
                         nullptr,
                         nullptr,
                         nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    const absl::Status& begun() const { return status_; }
    absl::Status commit()
    {
        absl::Status s = exec(db_, nested_ ? "RELEASE mutation" : "COMMIT");
        committed_ = s.ok();
        return s;
    }

private:
    sqlite3* db_;
    absl::Status status_;
    bool committed_{};
    bool nested_{};
};

#define CHRONOLOG_RETURN_IF_ERROR(expr)                                                                                \
    do {                                                                                                               \
        absl::Status status__ = (expr);                                                                                \
        if(!status__.ok())                                                                                             \
            return status__;                                                                                           \
    } while(false)

absl::StatusOr<uint64_t> nextCounter(sqlite3* db, const char* name)
{
    {
        Statement update(db, "UPDATE counters SET value = value + 1 WHERE name = ?1");
        CHRONOLOG_RETURN_IF_ERROR(update.prepared());
        update.text(1, name);
        auto row = update.step();
        if(!row.ok())
            return row.status();
    }
    Statement select(db, "SELECT value FROM counters WHERE name = ?1");
    CHRONOLOG_RETURN_IF_ERROR(select.prepared());
    select.text(1, name);
    auto row = select.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return absl::InternalError(absl::StrCat("missing counter ", name));
    return select.column(0);
}

absl::StatusOr<uint64_t> currentCounter(sqlite3* db, const char* name)
{
    Statement select(db, "SELECT value FROM counters WHERE name = ?1");
    CHRONOLOG_RETURN_IF_ERROR(select.prepared());
    select.text(1, name);
    auto row = select.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return absl::InternalError(absl::StrCat("missing counter ", name));
    return select.column(0);
}

struct ChronicleRow
{
    uint64_t id{};
    Chronicle chronicle;
};

// The live identity for a name, else the most recently destroyed one.
absl::StatusOr<std::optional<ChronicleRow>> loadChronicle(sqlite3* db, const std::string& name)
{
    Statement s(db,
                "SELECT id, name, tombstoned FROM chronicles WHERE name = ?1"
                " ORDER BY tombstoned ASC, id DESC LIMIT 1");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.text(1, name);
    auto row = s.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return std::optional<ChronicleRow>();
    return std::optional<ChronicleRow>(ChronicleRow{s.column(0), Chronicle{s.columnText(1), s.column(2) != 0}});
}

Story readStory(const Statement& s)
{
    return Story{s.column(0), s.columnText(1), s.columnText(2), s.column(3), s.column(4) != 0};
}

constexpr const char* kStoryColumns = "id, chronicle, name, epoch, tombstoned";

absl::StatusOr<std::optional<Story>> loadStory(sqlite3* db, StoryId id)
{
    Statement s(db, "SELECT id, chronicle, name, epoch, tombstoned FROM stories WHERE id = ?1");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.integer(1, id);
    auto row = s.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return std::optional<Story>();
    return std::optional<Story>(readStory(s));
}

absl::StatusOr<bool> storyHasActiveAcquisition(sqlite3* db, StoryId id)
{
    Statement s(db, "SELECT 1 FROM acquisitions WHERE story_id = ?1 AND released = 0 LIMIT 1");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.integer(1, id);
    return s.step();
}

} // namespace

absl::StatusOr<std::unique_ptr<SqliteMetadataStore>>
SqliteMetadataStore::open(const std::string& path, Topology topology, FenceWaiter fence_waiter)
{
    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if(sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK)
    {
        absl::Status s = absl::UnavailableError(
                absl::StrCat("cannot open catalog database ", path, ": ", db ? sqlite3_errmsg(db) : "out of memory"));
        sqlite3_close(db);
        return s;
    }
    std::unique_ptr<SqliteMetadataStore> store(
            new SqliteMetadataStore(db, std::move(topology), std::move(fence_waiter)));
    absl::Status init = store->initialize();
    if(!init.ok())
        return init;
    return store;
}

SqliteMetadataStore::SqliteMetadataStore(sqlite3* db, Topology topology, FenceWaiter fence_waiter)
    : db_(db)
    , topology_(std::move(topology))
    , fence_waiter_(std::move(fence_waiter))
{}

SqliteMetadataStore::~SqliteMetadataStore() { sqlite3_close(db_); }

absl::Status SqliteMetadataStore::initialize()
{
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "PRAGMA foreign_keys=ON"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "PRAGMA synchronous=FULL"));
    auto mode = pragmaValue("journal_mode=WAL");
    if(!mode.ok())
        return mode.status();
    if(*mode != "wal")
        return absl::UnavailableError(absl::StrCat("catalog database cannot use WAL journal mode, got ", *mode));

    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "CREATE TABLE IF NOT EXISTS membership_state(value TEXT NOT NULL)"));
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS chronicles("
                                   " id INTEGER PRIMARY KEY,"
                                   " name TEXT NOT NULL,"
                                   " tombstoned INTEGER NOT NULL DEFAULT 0)"));
    // A destroyed name can be created again with a new identity.
    CHRONOLOG_RETURN_IF_ERROR(
            exec(db_,
                 "CREATE UNIQUE INDEX IF NOT EXISTS chronicles_live_name ON chronicles(name) WHERE tombstoned = 0"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS stories("
                                   " id INTEGER PRIMARY KEY,"
                                   " chronicle_id INTEGER NOT NULL REFERENCES chronicles(id),"
                                   " chronicle TEXT NOT NULL,"
                                   " name TEXT NOT NULL,"
                                   " epoch INTEGER NOT NULL,"
                                   " tombstoned INTEGER NOT NULL DEFAULT 0)"));
    // SQLite has no table level UNIQUE ... WHERE, so live name uniqueness is a
    // partial unique index.
    CHRONOLOG_RETURN_IF_ERROR(exec(
            db_,
            "CREATE UNIQUE INDEX IF NOT EXISTS stories_live_name ON stories(chronicle_id, name) WHERE tombstoned = 0"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS writers("
                                   " writer_identity TEXT PRIMARY KEY,"
                                   " writer_id INTEGER NOT NULL UNIQUE)"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS acquisitions("
                                   " story_id INTEGER NOT NULL REFERENCES stories(id),"
                                   " writer_id INTEGER NOT NULL REFERENCES writers(writer_id),"
                                   " incarnation INTEGER NOT NULL,"
                                   " released INTEGER NOT NULL DEFAULT 0,"
                                   " keeper_id TEXT NOT NULL,"
                                   " keeper_endpoint TEXT NOT NULL,"
                                   " PRIMARY KEY(story_id, writer_id))"));
    // One row per committed release, so a retried release returns the revision it
    // first committed and never allocates another.
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS releases("
                                   " story_id INTEGER NOT NULL,"
                                   " writer_id INTEGER NOT NULL,"
                                   " incarnation INTEGER NOT NULL,"
                                   " revision INTEGER NOT NULL,"
                                   " keeper_id TEXT NOT NULL,"
                                   " keeper_endpoint TEXT NOT NULL,"
                                   " PRIMARY KEY(story_id, writer_id, incarnation))"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
                                   "CREATE TABLE IF NOT EXISTS counters("
                                   " name TEXT PRIMARY KEY,"
                                   " value INTEGER NOT NULL)"));
    CHRONOLOG_RETURN_IF_ERROR(
            exec(db_,
                 "INSERT OR IGNORE INTO counters(name, value) VALUES"
                 " ('story_id', 0), ('writer_id', 0), ('acquisition_revision', 0), ('raft_index', 0)"));

    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "CREATE TABLE IF NOT EXISTS raft_result(value TEXT NOT NULL)"));
    Statement version(db_, "SELECT version FROM schema_version LIMIT 1");
    CHRONOLOG_RETURN_IF_ERROR(version.prepared());
    auto row = version.step();
    if(!row.ok())
        return row.status();
    if(*row)
    {
        if(version.column(0) != 1 && version.column(0) != static_cast<uint64_t>(kSchemaVersion))
            return absl::FailedPreconditionError(
                    absl::StrCat("catalog schema version ", version.column(0), " is not supported"));
    }
    else
    {
        Statement insert(db_, "INSERT INTO schema_version(version) VALUES (?1)");
        CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
        insert.integer(1, static_cast<uint64_t>(kSchemaVersion));
        auto done = insert.step();
        if(!done.ok())
            return done.status();
    }
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "UPDATE schema_version SET version=2"));
    CHRONOLOG_RETURN_IF_ERROR(initializeMembership());
    return txn.commit();
}

absl::StatusOr<std::string> SqliteMetadataStore::pragmaValue(const std::string& name) const
{
    std::lock_guard lock(mutex_);
    Statement s(db_, ("PRAGMA " + name).c_str());
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    auto row = s.step();
    if(!row.ok())
        return row.status();
    return *row ? s.columnText(0) : std::string();
}

absl::StatusOr<Chronicle> SqliteMetadataStore::createChronicle(std::string name)
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto existing = loadChronicle(db_, name);
    if(!existing.ok())
        return existing.status();
    if(*existing && !(*existing)->chronicle.tombstoned)
        return absl::AlreadyExistsError("chronicle exists");
    Statement insert(db_, "INSERT INTO chronicles(name, tombstoned) VALUES (?1, 0)");
    CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
    insert.text(1, name);
    auto done = insert.step();
    if(!done.ok())
        return done.status();
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());
    return Chronicle{std::move(name), false};
}

absl::StatusOr<Chronicle> SqliteMetadataStore::getChronicle(std::string name) const
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    auto existing = loadChronicle(db_, name);
    if(!existing.ok())
        return existing.status();
    if(!*existing)
        return absl::NotFoundError("unknown chronicle");
    return (*existing)->chronicle;
}

absl::StatusOr<std::vector<Chronicle>> SqliteMetadataStore::listChronicles() const
{
    std::lock_guard lock(mutex_);
    Statement s(db_, "SELECT name, tombstoned FROM chronicles ORDER BY id");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    std::vector<Chronicle> out;
    while(true)
    {
        auto row = s.step();
        if(!row.ok())
            return row.status();
        if(!*row)
            break;
        out.push_back(Chronicle{s.columnText(0), s.column(1) != 0});
    }
    return out;
}

absl::Status SqliteMetadataStore::destroyChronicle(std::string name)
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto existing = loadChronicle(db_, name);
    if(!existing.ok())
        return existing.status();
    if(!*existing)
        return absl::NotFoundError("unknown chronicle");
    if((*existing)->chronicle.tombstoned)
        return absl::OkStatus();
    const uint64_t chronicle_id = (*existing)->id;
    {
        Statement active(db_,
                         "SELECT 1 FROM acquisitions a JOIN stories s ON s.id = a.story_id"
                         " WHERE s.chronicle_id = ?1 AND a.released = 0 LIMIT 1");
        CHRONOLOG_RETURN_IF_ERROR(active.prepared());
        active.integer(1, chronicle_id);
        auto row = active.step();
        if(!row.ok())
            return row.status();
        if(*row)
            return absl::FailedPreconditionError("story has an active acquisition");
    }
    std::vector<StoryId> destroyed;
    {
        Statement live(db_, "SELECT id FROM stories WHERE chronicle_id = ?1 AND tombstoned = 0 ORDER BY id");
        CHRONOLOG_RETURN_IF_ERROR(live.prepared());
        live.integer(1, chronicle_id);
        while(true)
        {
            auto row = live.step();
            if(!row.ok())
                return row.status();
            if(!*row)
                break;
            destroyed.push_back(live.column(0));
        }
    }
    CHRONOLOG_RETURN_IF_ERROR(tombstoneStories(destroyed));
    {
        Statement stories(db_, "UPDATE stories SET tombstoned = 1 WHERE chronicle_id = ?1");
        CHRONOLOG_RETURN_IF_ERROR(stories.prepared());
        stories.integer(1, chronicle_id);
        auto done = stories.step();
        if(!done.ok())
            return done.status();
    }
    Statement chronicle(db_, "UPDATE chronicles SET tombstoned = 1 WHERE id = ?1");
    CHRONOLOG_RETURN_IF_ERROR(chronicle.prepared());
    chronicle.integer(1, chronicle_id);
    auto done = chronicle.step();
    if(!done.ok())
        return done.status();
    return txn.commit();
}

absl::StatusOr<Story> SqliteMetadataStore::createStory(std::string chronicle, std::string name)
{
    if(!validName(chronicle) || !validName(name))
        return absl::InvalidArgumentError("invalid chronicle or story name");
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto parent = loadChronicle(db_, chronicle);
    if(!parent.ok())
        return parent.status();
    if(!*parent)
        return absl::NotFoundError("unknown chronicle");
    if((*parent)->chronicle.tombstoned)
        return absl::FailedPreconditionError("chronicle was destroyed");
    {
        Statement dup(db_, "SELECT 1 FROM stories WHERE chronicle_id = ?1 AND name = ?2 AND tombstoned = 0");
        CHRONOLOG_RETURN_IF_ERROR(dup.prepared());
        dup.integer(1, (*parent)->id).text(2, name);
        auto row = dup.step();
        if(!row.ok())
            return row.status();
        if(*row)
            return absl::AlreadyExistsError("story exists");
    }
    auto id = nextCounter(db_, "story_id");
    if(!id.ok())
        return id.status();
    Statement insert(db_,
                     "INSERT INTO stories(id, chronicle_id, chronicle, name, epoch, tombstoned)"
                     " VALUES (?1, ?2, ?3, ?4, ?5, 0)");
    CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
    insert.integer(1, *id).integer(2, (*parent)->id).text(3, chronicle).text(4, name).integer(5, kInitialEpoch);
    auto done = insert.step();
    if(!done.ok())
        return done.status();
    CHRONOLOG_RETURN_IF_ERROR(seedMembershipStory(*id));
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());
    return Story{*id, std::move(chronicle), std::move(name), kInitialEpoch, false};
}

absl::StatusOr<Story> SqliteMetadataStore::getStory(StoryId id) const
{
    std::lock_guard lock(mutex_);
    auto story = loadStory(db_, id);
    if(!story.ok())
        return story.status();
    if(!*story)
        return absl::NotFoundError("unknown story");
    return **story;
}

absl::StatusOr<std::vector<Story>> SqliteMetadataStore::listStories(std::string chronicle) const
{
    if(!validName(chronicle))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    auto parent = loadChronicle(db_, chronicle);
    if(!parent.ok())
        return parent.status();
    if(!*parent)
        return absl::NotFoundError("unknown chronicle");
    Statement s(db_,
                (std::string("SELECT ") + kStoryColumns + " FROM stories WHERE chronicle_id = ?1 ORDER BY id").c_str());
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.integer(1, (*parent)->id);
    std::vector<Story> out;
    while(true)
    {
        auto row = s.step();
        if(!row.ok())
            return row.status();
        if(!*row)
            break;
        out.push_back(readStory(s));
    }
    return out;
}

absl::Status SqliteMetadataStore::destroyStory(StoryId id)
{
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto story = loadStory(db_, id);
    if(!story.ok())
        return story.status();
    if(!*story)
        return absl::NotFoundError("unknown story");
    auto active = storyHasActiveAcquisition(db_, id);
    if(!active.ok())
        return active.status();
    if(*active)
        return absl::FailedPreconditionError("story has an active acquisition");
    if(!(*story)->tombstoned)
        CHRONOLOG_RETURN_IF_ERROR(tombstoneStories({id}));
    Statement update(db_, "UPDATE stories SET tombstoned = 1 WHERE id = ?1");
    CHRONOLOG_RETURN_IF_ERROR(update.prepared());
    update.integer(1, id);
    auto done = update.step();
    if(!done.ok())
        return done.status();
    return txn.commit();
}

void SqliteMetadataStore::setOwnerFence(FenceWaiter fence)
{
    std::lock_guard lock(mutex_);
    owner_fence_ = std::move(fence);
}

absl::Status SqliteMetadataStore::awaitOldOwnerFence(StoryId id, const std::string& writer_identity) const
{
    KeeperRef old_owner;
    uint64_t release_revision = 0;
    FenceWaiter fence;
    {
        std::lock_guard lock(mutex_);
        fence = owner_fence_;
        if(!fence)
            return absl::OkStatus();
        Statement find(db_,
                       "SELECT a.incarnation, a.released, a.keeper_id, a.keeper_endpoint, a.writer_id FROM writers w"
                       " JOIN acquisitions a ON a.writer_id = w.writer_id"
                       " WHERE w.writer_identity = ?1 AND a.story_id = ?2");
        CHRONOLOG_RETURN_IF_ERROR(find.prepared());
        find.text(1, writer_identity).integer(2, id);
        auto row = find.step();
        if(!row.ok())
            return row.status();
        if(!*row || find.column(1) == 0)
            return absl::OkStatus();
        old_owner = KeeperRef{find.columnText(2), find.columnText(3)};
        auto route = membershipRoute(id);
        if(!route.ok())
            return absl::OkStatus();
        for(const auto& keeper: route->keepers)
            if(keeper.process_id == old_owner.process_id)
                return absl::OkStatus();
        Statement released(db_,
                           "SELECT revision FROM releases WHERE story_id = ?1 AND writer_id = ?2 AND incarnation = ?3");
        CHRONOLOG_RETURN_IF_ERROR(released.prepared());
        released.integer(1, id).integer(2, find.column(4)).integer(3, find.column(0));
        auto revision = released.step();
        if(!revision.ok())
            return revision.status();
        if(!*revision)
            return absl::OkStatus();
        release_revision = released.column(0);
    }
    // The wait runs outside the store lock so a slow Keeper never stalls the Catalog.
    if(!fence(old_owner, release_revision))
        return absl::UnavailableError("the previous owner has not fenced the old incarnation");
    return absl::OkStatus();
}

absl::StatusOr<Acquisition> SqliteMetadataStore::acquire(StoryId id, std::string writer_identity)
{
    CHRONOLOG_RETURN_IF_ERROR(awaitOldOwnerFence(id, writer_identity));
    return acquireAfterFence(id, std::move(writer_identity));
}

absl::StatusOr<Acquisition> SqliteMetadataStore::acquireAfterFence(StoryId id, std::string writer_identity)
{
    if(writer_identity.empty())
        return absl::InvalidArgumentError("writer identity is empty");
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto story = loadStory(db_, id);
    if(!story.ok())
        return story.status();
    if(!*story)
        return absl::NotFoundError("unknown story");
    if((*story)->tombstoned)
        return absl::FailedPreconditionError("story was destroyed");

    uint64_t writer_id = 0;
    {
        Statement find(db_, "SELECT writer_id FROM writers WHERE writer_identity = ?1");
        CHRONOLOG_RETURN_IF_ERROR(find.prepared());
        find.text(1, writer_identity);
        auto row = find.step();
        if(!row.ok())
            return row.status();
        if(*row)
            writer_id = find.column(0);
    }
    if(writer_id == 0)
    {
        auto allocated = nextCounter(db_, "writer_id");
        if(!allocated.ok())
            return allocated.status();
        writer_id = *allocated;
        Statement insert(db_, "INSERT INTO writers(writer_identity, writer_id) VALUES (?1, ?2)");
        CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
        insert.text(1, writer_identity).integer(2, writer_id);
        auto done = insert.step();
        if(!done.ok())
            return done.status();
    }

    uint64_t incarnation = 1;
    std::optional<AcquisitionChange> superseded;
    {
        Statement prior(db_,
                        "SELECT incarnation, released, keeper_id, keeper_endpoint FROM acquisitions"
                        " WHERE story_id = ?1 AND writer_id = ?2");
        CHRONOLOG_RETURN_IF_ERROR(prior.prepared());
        prior.integer(1, id).integer(2, writer_id);
        auto row = prior.step();
        if(!row.ok())
            return row.status();
        if(*row)
        {
            incarnation = prior.column(0) + 1;
            // A writer that re-acquires while still active has crashed. Its old
            // incarnation is released in the same transaction as the new one.
            if(prior.column(1) == 0)
            {
                auto released_revision = nextCounter(db_, "acquisition_revision");
                if(!released_revision.ok())
                    return released_revision.status();
                superseded = AcquisitionChange{*released_revision,
                                               id,
                                               writer_id,
                                               prior.column(0),
                                               KeeperRef{prior.columnText(2), prior.columnText(3)},
                                               AcquisitionState::Released};
            }
        }
    }
    if(superseded)
    {
        Statement record(db_,
                         "INSERT INTO releases(story_id, writer_id, incarnation, revision, keeper_id, keeper_endpoint)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        CHRONOLOG_RETURN_IF_ERROR(record.prepared());
        record.integer(1, id)
                .integer(2, writer_id)
                .integer(3, superseded->incarnation)
                .integer(4, superseded->revision)
                .text(5, superseded->assigned_keeper.process_id)
                .text(6, superseded->assigned_keeper.endpoint);
        auto done = record.step();
        if(!done.ok())
            return done.status();
    }
    auto owning_route = membershipRoute(id);
    if(!owning_route.ok())
        return owning_route.status();
    absl::StatusOr<KeeperRef> keeper = absl::FailedPreconditionError("route has no keepers");
    if(!owning_route->keepers.empty())
        keeper = owning_route->keepers[writer_id % owning_route->keepers.size()];
    Statement existing(db_, "SELECT keeper_id,keeper_endpoint FROM acquisitions WHERE story_id=?1 AND writer_id=?2");
    CHRONOLOG_RETURN_IF_ERROR(existing.prepared());
    existing.integer(1, id).integer(2, writer_id);
    auto prior_keeper = existing.step();
    if(!prior_keeper.ok())
        return prior_keeper.status();
    if(*prior_keeper)
        for(const auto& k: owning_route->keepers)
            if(k.process_id == existing.columnText(0))
                keeper = k;

    if(!keeper.ok())
        return keeper.status();
    {
        Statement upsert(db_,
                         "INSERT OR REPLACE INTO acquisitions(story_id, writer_id, incarnation, released, keeper_id,"
                         " keeper_endpoint) VALUES (?1, ?2, ?3, 0, ?4, ?5)");
        CHRONOLOG_RETURN_IF_ERROR(upsert.prepared());
        upsert.integer(1, id)
                .integer(2, writer_id)
                .integer(3, incarnation)
                .text(4, keeper->process_id)
                .text(5, keeper->endpoint);
        auto done = upsert.step();
        if(!done.ok())
            return done.status();
    }
    auto revision = nextCounter(db_, "acquisition_revision");
    if(!revision.ok())
        return revision.status();
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());

    if(observer_)
    {
        if(superseded)
            observer_->onAcquisitionChange(*superseded);
        observer_->onAcquisitionChange({*revision, id, writer_id, incarnation, *keeper, AcquisitionState::Acquired});
    }
    return Acquisition{id, writer_id, incarnation, *owning_route, *keeper};
}

absl::StatusOr<ReleaseResult> SqliteMetadataStore::release(StoryId id, uint64_t writer_id, uint64_t incarnation)
{
    AcquisitionChange change;
    {
        std::lock_guard lock(mutex_);
        Transaction txn(db_);
        CHRONOLOG_RETURN_IF_ERROR(txn.begun());
        KeeperRef keeper;
        bool retried = false;
        {
            Statement done_before(db_,
                                  "SELECT revision, keeper_id, keeper_endpoint FROM releases"
                                  " WHERE story_id = ?1 AND writer_id = ?2 AND incarnation = ?3");
            CHRONOLOG_RETURN_IF_ERROR(done_before.prepared());
            done_before.integer(1, id).integer(2, writer_id).integer(3, incarnation);
            auto row = done_before.step();
            if(!row.ok())
                return row.status();
            if(*row)
            {
                retried = true;
                change = {done_before.column(0),
                          id,
                          writer_id,
                          incarnation,
                          KeeperRef{done_before.columnText(1), done_before.columnText(2)},
                          AcquisitionState::Released};
            }
        }
        if(!retried)
        {
            {
                Statement find(db_,
                               "SELECT incarnation, released, keeper_id, keeper_endpoint FROM acquisitions"
                               " WHERE story_id = ?1 AND writer_id = ?2");
                CHRONOLOG_RETURN_IF_ERROR(find.prepared());
                find.integer(1, id).integer(2, writer_id);
                auto row = find.step();
                if(!row.ok())
                    return row.status();
                if(!*row || find.column(0) < incarnation)
                    return absl::NotFoundError("unknown acquisition");
                if(find.column(0) != incarnation || find.column(1) != 0)
                    return absl::FailedPreconditionError("incarnation is stale");
                keeper = KeeperRef{find.columnText(2), find.columnText(3)};
            }
            {
                Statement update(db_, "UPDATE acquisitions SET released = 1 WHERE story_id = ?1 AND writer_id = ?2");
                CHRONOLOG_RETURN_IF_ERROR(update.prepared());
                update.integer(1, id).integer(2, writer_id);
                auto done = update.step();
                if(!done.ok())
                    return done.status();
            }
            auto revision = nextCounter(db_, "acquisition_revision");
            if(!revision.ok())
                return revision.status();
            {
                Statement record(db_,
                                 "INSERT INTO releases(story_id, writer_id, incarnation, revision, keeper_id,"
                                 " keeper_endpoint) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
                CHRONOLOG_RETURN_IF_ERROR(record.prepared());
                record.integer(1, id)
                        .integer(2, writer_id)
                        .integer(3, incarnation)
                        .integer(4, *revision)
                        .text(5, keeper.process_id)
                        .text(6, keeper.endpoint);
                auto done = record.step();
                if(!done.ok())
                    return done.status();
            }
            CHRONOLOG_RETURN_IF_ERROR(txn.commit());
            change = {*revision, id, writer_id, incarnation, std::move(keeper), AcquisitionState::Released};
            if(observer_)
                observer_->onAcquisitionChange(change);
        }
    }
    // The wait happens outside the store lock so a slow Keeper never stalls the Catalog.
    const bool fenced = fence_waiter_ && fence_waiter_(change.assigned_keeper, change.revision);
    return ReleaseResult{fenced, change.revision};
}

absl::StatusOr<Epoch> SqliteMetadataStore::compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired)
{
    if(desired <= expected)
        return absl::InvalidArgumentError("desired epoch must exceed expected");
    std::lock_guard lock(mutex_);
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto story = loadStory(db_, id);
    if(!story.ok())
        return story.status();
    if(!*story)
        return absl::NotFoundError("unknown story");
    if((*story)->tombstoned)
        return absl::FailedPreconditionError("story was destroyed");
    if((*story)->epoch != expected)
        return absl::FailedPreconditionError("epoch mismatch");
    Statement update(db_, "UPDATE stories SET epoch = ?2 WHERE id = ?1");
    CHRONOLOG_RETURN_IF_ERROR(update.prepared());
    update.integer(1, id).integer(2, desired);
    auto done = update.step();
    if(!done.ok())
        return done.status();
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());
    return desired;
}

absl::StatusOr<AcquisitionSnapshot> SqliteMetadataStore::snapshotAcquisitions() const
{
    std::lock_guard lock(mutex_);
    AcquisitionSnapshot snapshot;
    auto revision = currentCounter(db_, "acquisition_revision");
    if(!revision.ok())
        return revision.status();
    snapshot.revision = *revision;
    Statement s(db_,
                "SELECT story_id, writer_id, incarnation, keeper_id, keeper_endpoint FROM acquisitions"
                " WHERE released = 0 ORDER BY story_id, writer_id");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    while(true)
    {
        auto row = s.step();
        if(!row.ok())
            return row.status();
        if(!*row)
            break;
        snapshot.active.push_back({snapshot.revision,
                                   s.column(0),
                                   s.column(1),
                                   s.column(2),
                                   KeeperRef{s.columnText(3), s.columnText(4)},
                                   AcquisitionState::Acquired});
    }
    return snapshot;
}

void SqliteMetadataStore::setObserver(AcquisitionObserver* observer)
{
    std::lock_guard lock(mutex_);
    observer_ = observer;
}

absl::StatusOr<KeeperRef> SqliteMetadataStore::releasedKeeper(StoryId id, uint64_t writer, uint64_t incarnation) const
{
    std::lock_guard lock(mutex_);
    Statement q(db_,
                "SELECT keeper_id,keeper_endpoint FROM releases WHERE story_id=?1 AND writer_id=?2 AND incarnation=?3");
    CHRONOLOG_RETURN_IF_ERROR(q.prepared());
    q.integer(1, id).integer(2, writer).integer(3, incarnation);
    auto row = q.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return absl::NotFoundError("release not found");
    return KeeperRef{q.columnText(0), q.columnText(1)};
}
int64_t SqliteMetadataStore::totalChanges() const
{
    std::lock_guard lock(mutex_);
    return sqlite3_total_changes64(db_);
}

absl::StatusOr<uint64_t> SqliteMetadataStore::appliedIndex() const
{
    std::lock_guard lock(mutex_);
    return currentCounter(db_, "raft_index");
}
absl::StatusOr<std::string> SqliteMetadataStore::applyRaft(uint64_t index, const std::function<std::string()>& apply)
{
    std::lock_guard lock(mutex_);
    auto prior = currentCounter(db_, "raft_index");
    if(!prior.ok())
        return prior.status();
    if(index <= *prior)
    {
        Statement q(db_, "SELECT value FROM raft_result LIMIT 1");
        CHRONOLOG_RETURN_IF_ERROR(q.prepared());
        auto row = q.step();
        if(!row.ok())
            return row.status();
        return *row ? q.columnText(0) : std::string();
    }
    struct Pending final: AcquisitionObserver
    {
        std::vector<AcquisitionChange> changes;
        void onAcquisitionChange(const AcquisitionChange& c) override { changes.push_back(c); }
    } pending;
    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    auto revision = nextCounter(db_, "acquisition_revision");
    if(!revision.ok())
        return revision.status();
    auto* observer = observer_;
    observer_ = &pending;
    apply_revision_ = *revision;
    std::string result;
    try
    {
        result = apply();
    }
    catch(...)
    {
        observer_ = observer;
        apply_revision_.reset();
        throw;
    }
    observer_ = observer;
    apply_revision_.reset();
    Statement update(db_, "UPDATE counters SET value=?1 WHERE name='raft_index'");
    CHRONOLOG_RETURN_IF_ERROR(update.prepared());
    update.integer(1, index);
    auto done = update.step();
    if(!done.ok())
        return done.status();
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "DELETE FROM raft_result"));
    Statement insert(db_, "INSERT INTO raft_result VALUES(?1)");
    CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
    insert.text(1, result);
    done = insert.step();
    if(!done.ok())
        return done.status();
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());
    if(observer)
        for(const auto& c: pending.changes) observer->onAcquisitionChange(c);
    return result;
}
absl::Status SqliteMetadataStore::backupTo(const std::string& path) const
{
    std::lock_guard lock(mutex_);
    sqlite3* target{};
    if(sqlite3_open(path.c_str(), &target) != SQLITE_OK)
    {
        if(target)
            sqlite3_close(target);
        return absl::UnavailableError("snapshot open failed");
    }
    auto* backup = sqlite3_backup_init(target, "main", db_, "main");
    int rc = backup ? sqlite3_backup_step(backup, -1) : SQLITE_ERROR;
    if(backup)
        sqlite3_backup_finish(backup);
    sqlite3_close(target);
    return rc == SQLITE_DONE ? absl::OkStatus() : absl::UnavailableError("snapshot backup failed");
}
absl::Status SqliteMetadataStore::installFrom(const std::string& path)
{
    std::lock_guard lock(mutex_);
    const std::string destination = sqlite3_db_filename(db_, "main");
    const std::string temporary = destination + ".install";
    std::error_code error;
    std::filesystem::copy_file(path, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if(error)
        return absl::UnavailableError(error.message());
    int fd = ::open(temporary.c_str(), O_RDONLY);
    if(fd < 0)
        return absl::UnavailableError("snapshot open failed");
    int rc = fsync(fd);
    close(fd);
    if(rc)
        return absl::UnavailableError("snapshot fsync failed");
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "PRAGMA wal_checkpoint(TRUNCATE)"));
    sqlite3_close(db_);
    db_ = nullptr;
    std::filesystem::rename(temporary, destination, error);
    if(error)
    {
        sqlite3_open(destination.c_str(), &db_);
        return absl::UnavailableError(error.message());
    }
    const auto parent = std::filesystem::path(destination).parent_path();
    fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
    rc = fd < 0 ? -1 : fsync(fd);
    if(fd >= 0)
        close(fd);
    if(sqlite3_open(destination.c_str(), &db_) != SQLITE_OK)
        return absl::UnavailableError("snapshot reopen failed");
    CHRONOLOG_RETURN_IF_ERROR(initialize());
    snapshot_generation_.fetch_add(1);
    return rc == 0 ? absl::OkStatus() : absl::UnavailableError("snapshot directory fsync failed");
}

absl::StatusOr<Route> SqliteMetadataStore::membershipRoute(StoryId id) const
{
    std::lock_guard lock(mutex_);
    auto story = getStory(id);
    if(!story.ok())
        return story.status();
    if(story->tombstoned)
        return absl::NotFoundError("destroyed story");
    auto update = membershipRouteUpdate(id);
    if(update.ok())
    {
        Route route{update->route().epoch(), {}, update->route().grapher(), update->route().player()};
        for(const auto& k: update->route().keepers()) route.keepers.push_back({k.process_id(), k.endpoint()});
        return route;
    }
    if(!absl::IsNotFound(update.status()))
        return update.status();
    return topology_.routeFor(story->epoch, id);
}
absl::Status SqliteMetadataStore::fenceRemovedWriters(StoryId id,
                                                      const Route& route,
                                                      uint64_t revision,
                                                      const std::string& replacement)
{
    std::lock_guard lock(mutex_);
    auto active = storyAcquisitions(id);
    if(!active.ok())
        return active.status();
    for(auto change: *active)
    {
        bool survives = false;
        for(const auto& k: route.keepers)
            if(k.process_id == change.assigned_keeper.process_id)
                survives = true;
        if(survives && change.assigned_keeper.process_id != replacement)
            continue;
        Statement record(
                db_,
                "INSERT OR IGNORE INTO releases(story_id,writer_id,incarnation,revision,keeper_id,keeper_endpoint) "
                "VALUES (?1,?2,?3,?4,?5,?6)");
        CHRONOLOG_RETURN_IF_ERROR(record.prepared());
        record.integer(1, id)
                .integer(2, change.writer_id)
                .integer(3, change.incarnation)
                .integer(4, revision)
                .text(5, change.assigned_keeper.process_id)
                .text(6, change.assigned_keeper.endpoint);
        auto done = record.step();
        if(!done.ok())
            return done.status();
        Statement fence(db_, "UPDATE acquisitions SET released=1 WHERE story_id=?1 AND writer_id=?2");
        CHRONOLOG_RETURN_IF_ERROR(fence.prepared());
        fence.integer(1, id).integer(2, change.writer_id);
        done = fence.step();
        if(!done.ok())
            return done.status();
        change.revision = revision;
        change.state = AcquisitionState::Released;
        if(observer_)
            observer_->onAcquisitionChange(change);
    }
    return absl::OkStatus();
}
} // namespace chronolog::visor
