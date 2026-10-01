#include "catalog/SqliteMetadataStore.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "absl/strings/str_cat.h"

namespace chronolog::visor
{

namespace
{

constexpr int kSchemaVersion = 1;
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
        return raw ? std::string(raw) : std::string();
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
        status_ = exec(db_, "BEGIN IMMEDIATE");
    }
    ~Transaction()
    {
        if(status_.ok() && !committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    const absl::Status& begun() const { return status_; }
    absl::Status commit()
    {
        absl::Status s = exec(db_, "COMMIT");
        committed_ = s.ok();
        return s;
    }

private:
    sqlite3* db_;
    absl::Status status_;
    bool committed_{};
};

#define CHRONOLOG_RETURN_IF_ERROR(expr)   \
    do                                    \
    {                                     \
        absl::Status status__ = (expr);   \
        if(!status__.ok())                \
            return status__;              \
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

absl::StatusOr<std::optional<Chronicle>> loadChronicle(sqlite3* db, const std::string& name)
{
    Statement s(db, "SELECT name, tombstoned FROM chronicles WHERE name = ?1");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.text(1, name);
    auto row = s.step();
    if(!row.ok())
        return row.status();
    if(!*row)
        return std::optional<Chronicle>();
    return std::optional<Chronicle>(Chronicle{s.columnText(0), s.column(1) != 0});
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

absl::StatusOr<std::unique_ptr<SqliteMetadataStore>> SqliteMetadataStore::open(const std::string& path,
                                                                                 Topology topology,
                                                                                 FenceWaiter fence_waiter)
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
    std::unique_ptr<SqliteMetadataStore> store(new SqliteMetadataStore(db, std::move(topology), std::move(fence_waiter)));
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

SqliteMetadataStore::~SqliteMetadataStore()
{
    sqlite3_close(db_);
}

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

    Transaction txn(db_);
    CHRONOLOG_RETURN_IF_ERROR(txn.begun());
    CHRONOLOG_RETURN_IF_ERROR(exec(db_, "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
        "CREATE TABLE IF NOT EXISTS chronicles("
        " name TEXT PRIMARY KEY,"
        " tombstoned INTEGER NOT NULL DEFAULT 0)"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
        "CREATE TABLE IF NOT EXISTS stories("
        " id INTEGER PRIMARY KEY,"
        " chronicle TEXT NOT NULL REFERENCES chronicles(name),"
        " name TEXT NOT NULL,"
        " epoch INTEGER NOT NULL,"
        " tombstoned INTEGER NOT NULL DEFAULT 0)"));
    // SQLite has no table level UNIQUE ... WHERE, so live name uniqueness is a
    // partial unique index.
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
        "CREATE UNIQUE INDEX IF NOT EXISTS stories_live_name ON stories(chronicle, name) WHERE tombstoned = 0"));
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
        " assigned_keeper TEXT NOT NULL,"
        " PRIMARY KEY(story_id, writer_id))"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
        "CREATE TABLE IF NOT EXISTS counters("
        " name TEXT PRIMARY KEY,"
        " value INTEGER NOT NULL)"));
    CHRONOLOG_RETURN_IF_ERROR(exec(db_,
        "INSERT OR IGNORE INTO counters(name, value) VALUES"
        " ('story_id', 0), ('writer_id', 0), ('acquisition_revision', 0)"));

    Statement version(db_, "SELECT version FROM schema_version LIMIT 1");
    CHRONOLOG_RETURN_IF_ERROR(version.prepared());
    auto row = version.step();
    if(!row.ok())
        return row.status();
    if(*row)
    {
        if(version.column(0) != static_cast<uint64_t>(kSchemaVersion))
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
    if(*existing)
    {
        return (*existing)->tombstoned ? absl::FailedPreconditionError("chronicle was destroyed")
                                       : absl::AlreadyExistsError("chronicle exists");
    }
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
    return **existing;
}

absl::StatusOr<std::vector<Chronicle>> SqliteMetadataStore::listChronicles() const
{
    std::lock_guard lock(mutex_);
    Statement s(db_, "SELECT name, tombstoned FROM chronicles ORDER BY name");
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
    {
        Statement active(db_,
                         "SELECT 1 FROM acquisitions a JOIN stories s ON s.id = a.story_id"
                         " WHERE s.chronicle = ?1 AND a.released = 0 LIMIT 1");
        CHRONOLOG_RETURN_IF_ERROR(active.prepared());
        active.text(1, name);
        auto row = active.step();
        if(!row.ok())
            return row.status();
        if(*row)
            return absl::FailedPreconditionError("story has an active acquisition");
    }
    {
        Statement stories(db_, "UPDATE stories SET tombstoned = 1 WHERE chronicle = ?1");
        CHRONOLOG_RETURN_IF_ERROR(stories.prepared());
        stories.text(1, name);
        auto done = stories.step();
        if(!done.ok())
            return done.status();
    }
    Statement chronicle(db_, "UPDATE chronicles SET tombstoned = 1 WHERE name = ?1");
    CHRONOLOG_RETURN_IF_ERROR(chronicle.prepared());
    chronicle.text(1, name);
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
    if((*parent)->tombstoned)
        return absl::FailedPreconditionError("chronicle was destroyed");
    {
        Statement dup(db_, "SELECT tombstoned FROM stories WHERE chronicle = ?1 AND name = ?2");
        CHRONOLOG_RETURN_IF_ERROR(dup.prepared());
        dup.text(1, chronicle).text(2, name);
        auto row = dup.step();
        if(!row.ok())
            return row.status();
        if(*row)
        {
            return dup.column(0) != 0 ? absl::FailedPreconditionError("story was destroyed")
                                      : absl::AlreadyExistsError("story exists");
        }
    }
    auto id = nextCounter(db_, "story_id");
    if(!id.ok())
        return id.status();
    Statement insert(db_, "INSERT INTO stories(id, chronicle, name, epoch, tombstoned) VALUES (?1, ?2, ?3, ?4, 0)");
    CHRONOLOG_RETURN_IF_ERROR(insert.prepared());
    insert.integer(1, *id).text(2, chronicle).text(3, name).integer(4, kInitialEpoch);
    auto done = insert.step();
    if(!done.ok())
        return done.status();
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
    Statement s(db_, (std::string("SELECT ") + kStoryColumns + " FROM stories WHERE chronicle = ?1 ORDER BY id").c_str());
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    s.text(1, chronicle);
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
    Statement update(db_, "UPDATE stories SET tombstoned = 1 WHERE id = ?1");
    CHRONOLOG_RETURN_IF_ERROR(update.prepared());
    update.integer(1, id);
    auto done = update.step();
    if(!done.ok())
        return done.status();
    return txn.commit();
}

absl::StatusOr<Acquisition> SqliteMetadataStore::acquire(StoryId id, std::string writer_identity)
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
    {
        Statement prior(db_, "SELECT incarnation FROM acquisitions WHERE story_id = ?1 AND writer_id = ?2");
        CHRONOLOG_RETURN_IF_ERROR(prior.prepared());
        prior.integer(1, id).integer(2, writer_id);
        auto row = prior.step();
        if(!row.ok())
            return row.status();
        // A writer that re-acquires while still active supersedes its previous
        // incarnation, which is how a crashed writer recovers.
        if(*row)
            incarnation = prior.column(0) + 1;
    }
    auto keeper = topology_.assignKeeper(writer_id, (*story)->epoch);
    if(!keeper.ok())
        return keeper.status();
    {
        Statement upsert(db_,
                         "INSERT OR REPLACE INTO acquisitions(story_id, writer_id, incarnation, released, assigned_keeper)"
                         " VALUES (?1, ?2, ?3, 0, ?4)");
        CHRONOLOG_RETURN_IF_ERROR(upsert.prepared());
        upsert.integer(1, id).integer(2, writer_id).integer(3, incarnation).text(4, *keeper);
        auto done = upsert.step();
        if(!done.ok())
            return done.status();
    }
    auto revision = nextCounter(db_, "acquisition_revision");
    if(!revision.ok())
        return revision.status();
    CHRONOLOG_RETURN_IF_ERROR(txn.commit());

    if(observer_)
        observer_->onAcquisitionChange({*revision, id, writer_id, incarnation, *keeper, AcquisitionState::Acquired});
    return Acquisition{id, writer_id, incarnation, topology_.routeFor((*story)->epoch), *keeper};
}

absl::StatusOr<ReleaseResult> SqliteMetadataStore::release(StoryId id, uint64_t writer_id, uint64_t incarnation)
{
    AcquisitionChange change;
    {
        std::lock_guard lock(mutex_);
        Transaction txn(db_);
        CHRONOLOG_RETURN_IF_ERROR(txn.begun());
        std::string keeper;
        {
            Statement find(db_,
                           "SELECT incarnation, released, assigned_keeper FROM acquisitions"
                           " WHERE story_id = ?1 AND writer_id = ?2");
            CHRONOLOG_RETURN_IF_ERROR(find.prepared());
            find.integer(1, id).integer(2, writer_id);
            auto row = find.step();
            if(!row.ok())
                return row.status();
            if(!*row)
                return absl::NotFoundError("unknown acquisition");
            if(find.column(1) != 0 || find.column(0) != incarnation)
                return absl::FailedPreconditionError("incarnation is stale or already released");
            keeper = find.columnText(2);
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
        CHRONOLOG_RETURN_IF_ERROR(txn.commit());
        change = {*revision, id, writer_id, incarnation, std::move(keeper), AcquisitionState::Released};
        if(observer_)
            observer_->onAcquisitionChange(change);
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
                "SELECT story_id, writer_id, incarnation, assigned_keeper FROM acquisitions"
                " WHERE released = 0 ORDER BY story_id, writer_id");
    CHRONOLOG_RETURN_IF_ERROR(s.prepared());
    while(true)
    {
        auto row = s.step();
        if(!row.ok())
            return row.status();
        if(!*row)
            break;
        snapshot.active.push_back(
                {snapshot.revision, s.column(0), s.column(1), s.column(2), s.columnText(3), AcquisitionState::Acquired});
    }
    return snapshot;
}

void SqliteMetadataStore::setObserver(AcquisitionObserver* observer)
{
    std::lock_guard lock(mutex_);
    observer_ = observer;
}

} // namespace chronolog::visor
