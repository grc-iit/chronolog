#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>

#include <sqlite3.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <SqliteCatalogStore.h>

namespace chronolog
{

namespace
{

// SQLite stores 64-bit integers signed; the ids are unsigned name hashes.
sqlite3_int64 toSql(uint64_t id) { return static_cast<sqlite3_int64>(id); }

uint64_t fromSql(sqlite3_int64 value) { return static_cast<uint64_t>(value); }

class Statement
{
public:
    Statement(sqlite3* db, char const* sql) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(Statement const&) = delete;
    Statement& operator=(Statement const&) = delete;

    bool prepared() const { return rc_ == SQLITE_OK; }

    Statement& text(int index, std::string const& value)
    {
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& integer(int index, sqlite3_int64 value)
    {
        sqlite3_bind_int64(stmt_, index, value);
        return *this;
    }
    // SQLITE_ROW, SQLITE_DONE or an extended error code.
    int step() { return rc_ = sqlite3_step(stmt_); }
    int rc() const { return rc_; }

    sqlite3_int64 column(int index) const { return sqlite3_column_int64(stmt_, index); }
    std::string columnText(int index) const
    {
        auto const* raw = reinterpret_cast<char const*>(sqlite3_column_text(stmt_, index));
        return raw ? std::string(raw, static_cast<size_t>(sqlite3_column_bytes(stmt_, index))) : std::string();
    }

    bool columnIsNull(int index) const { return sqlite3_column_type(stmt_, index) == SQLITE_NULL; }

private:
    sqlite3_stmt* stmt_ = nullptr;
    int rc_ = SQLITE_OK;
};

bool exec(sqlite3* db, char const* sql)
{
    char* message = nullptr;
    if(sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK)
    {
        LOG_ERROR("[SqliteCatalogStore] '{}' failed: {}", sql, message ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return false;
    }
    return true;
}

// BEGIN IMMEDIATE ... COMMIT. Rolls back on destruction unless commit() succeeded.
class Transaction
{
public:
    explicit Transaction(sqlite3* db)
        : db_(db)
    {
        begun_ = exec(db_, "BEGIN IMMEDIATE");
    }
    ~Transaction()
    {
        if(begun_ && !committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(Transaction const&) = delete;
    Transaction& operator=(Transaction const&) = delete;

    bool begun() const { return begun_; }
    bool commit()
    {
        committed_ = exec(db_, "COMMIT");
        return committed_;
    }

private:
    sqlite3* db_;
    bool begun_ = false;
    bool committed_ = false;
};

// The status of a failed insert, from its extended result code.
int insertStatus(int rc, int exists_status)
{
    switch(rc)
    {
        case SQLITE_CONSTRAINT_PRIMARYKEY:
        case SQLITE_CONSTRAINT_UNIQUE:
            return exists_status;
        case SQLITE_CONSTRAINT_FOREIGNKEY:
            return CL_ERR_NOT_EXIST;
        default:
            return CL_ERR_UNKNOWN;
    }
}

} // namespace

SqliteCatalogStore::SqliteCatalogStore(std::string path, int busy_timeout_ms)
    : path_(std::move(path))
    , busyTimeoutMs_(busy_timeout_ms)
{}

SqliteCatalogStore::~SqliteCatalogStore()
{
    if(db_ != nullptr)
        sqlite3_close(db_);
}

int SqliteCatalogStore::open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ != nullptr)
        return CL_SUCCESS;
    int const status = openLocked();
    if(status != CL_SUCCESS && db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    return status;
}

void SqliteCatalogStore::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ != nullptr)
        sqlite3_close(db_);
    db_ = nullptr;
}

int SqliteCatalogStore::openLocked()
{
    std::filesystem::path const parent = std::filesystem::path(path_).parent_path();
    if(!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if(ec)
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot create the catalog directory {}: {}", parent.string(), ec.message());
            return CL_ERR_INVALID_CONF;
        }
    }

    if(sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        LOG_ERROR("[SqliteCatalogStore] Cannot open the catalog database {}: {}",
                  path_,
                  db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory");
        return CL_ERR_INVALID_CONF;
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, busyTimeoutMs_);

    // EXCLUSIVE before WAL: the WAL index then lives in heap memory, and the
    // lock taken on the first access is held until the connection closes.
    if(!exec(db_, "PRAGMA locking_mode=EXCLUSIVE") || !exec(db_, "PRAGMA foreign_keys=ON") ||
       !exec(db_, "PRAGMA synchronous=FULL"))
        return CL_ERR_INVALID_CONF;
    {
        Statement mode(db_, "PRAGMA journal_mode=WAL");
        int const rc = mode.prepared() ? mode.step() : mode.rc();
        if(rc != SQLITE_ROW)
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot set WAL journal mode on {}: {}", path_, sqlite3_errmsg(db_));
            return CL_ERR_INVALID_CONF;
        }
        if(mode.columnText(0) != "wal")
        {
            LOG_ERROR("[SqliteCatalogStore] The catalog database {} cannot use WAL journal mode, got '{}'",
                      path_,
                      mode.columnText(0));
            return CL_ERR_INVALID_CONF;
        }
    }

    Transaction txn(db_);
    if(!txn.begun() || !exec(db_, "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)"))
        return CL_ERR_INVALID_CONF;
    {
        Statement version(db_, "SELECT version FROM schema_version LIMIT 1");
        int const rc = version.prepared() ? version.step() : version.rc();
        if(rc == SQLITE_ROW)
        {
            sqlite3_int64 const stored = version.column(0);
            if(stored > SCHEMA_VERSION)
            {
                LOG_ERROR("[SqliteCatalogStore] The catalog database {} has schema version {}, newer than this "
                          "visor's {}. Refusing to open it.",
                          path_,
                          stored,
                          SCHEMA_VERSION);
                return CL_ERR_INVALID_CONF;
            }
            if(stored < 1)
            {
                LOG_ERROR("[SqliteCatalogStore] The catalog database {} has an invalid schema version {}",
                          path_,
                          stored);
                return CL_ERR_INVALID_CONF;
            }
        }
        else if(rc == SQLITE_DONE)
        {
            Statement insert(db_, "INSERT INTO schema_version(version) VALUES (?1)");
            if(!insert.prepared() || insert.integer(1, SCHEMA_VERSION).step() != SQLITE_DONE)
            {
                LOG_ERROR("[SqliteCatalogStore] Cannot write the schema version to {}: {}", path_, sqlite3_errmsg(db_));
                return CL_ERR_INVALID_CONF;
            }
        }
        else
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot read the schema version of {}: {}", path_, sqlite3_errmsg(db_));
            return CL_ERR_INVALID_CONF;
        }
    }
    if(!exec(db_,
             "CREATE TABLE IF NOT EXISTS chronicles("
             " id INTEGER PRIMARY KEY,"
             " name TEXT UNIQUE NOT NULL,"
             " destroy_pending INTEGER NOT NULL DEFAULT 0)") ||
       !exec(db_,
             "CREATE TABLE IF NOT EXISTS stories("
             " id INTEGER PRIMARY KEY,"
             " chronicle_id INTEGER NOT NULL REFERENCES chronicles(id),"
             " name TEXT NOT NULL,"
             " destroy_pending INTEGER NOT NULL DEFAULT 0,"
             " UNIQUE(chronicle_id, name))") ||
       !txn.commit())
        return CL_ERR_INVALID_CONF;

    LOG_INFO("[SqliteCatalogStore] Opened the catalog database {} (schema version {})", path_, SCHEMA_VERSION);
    return CL_SUCCESS;
}

int SqliteCatalogStore::load(CatalogSnapshot& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    out = CatalogSnapshot();
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;

    CatalogSnapshot snapshot;
    {
        Statement select(db_, "SELECT id, name, destroy_pending FROM chronicles");
        int rc = select.prepared() ? select.step() : select.rc();
        for(; rc == SQLITE_ROW; rc = select.step())
        {
            CatalogChronicle chronicle{fromSql(select.column(0)), select.columnText(1), select.column(2) != 0};
            if(!catalogIdsMatch(chronicle))
            {
                LOG_ERROR("[SqliteCatalogStore] Chronicle '{}' is stored with id {}, but its name hashes to {}. "
                          "Refusing to load the catalog {}.",
                          chronicle.name,
                          chronicle.id,
                          catalogChronicleIdOf(chronicle.name),
                          path_);
                return CL_ERR_INVALID_CONF;
            }
            snapshot.chronicles.push_back(std::move(chronicle));
        }
        if(rc != SQLITE_DONE)
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot read the chronicles of {}: {}", path_, sqlite3_errmsg(db_));
            return CL_ERR_INVALID_CONF;
        }
    }
    {
        Statement select(db_,
                         "SELECT s.id, c.name, s.name, s.destroy_pending, s.chronicle_id"
                         " FROM stories s LEFT JOIN chronicles c ON c.id = s.chronicle_id");
        int rc = select.prepared() ? select.step() : select.rc();
        for(; rc == SQLITE_ROW; rc = select.step())
        {
            if(select.columnIsNull(1))
            {
                LOG_ERROR("[SqliteCatalogStore] Story '{}' (id {}) names chronicle id {}, which is not stored. "
                          "Refusing to load the catalog {}.",
                          select.columnText(2),
                          fromSql(select.column(0)),
                          fromSql(select.column(4)),
                          path_);
                return CL_ERR_INVALID_CONF;
            }
            CatalogStory story{fromSql(select.column(0)),
                               select.columnText(1),
                               select.columnText(2),
                               select.column(3) != 0};
            if(!catalogIdsMatch(story))
            {
                LOG_ERROR("[SqliteCatalogStore] Story '{}' of chronicle '{}' is stored with id {}, but its names "
                          "hash to {}. Refusing to load the catalog {}.",
                          story.story,
                          story.chronicle,
                          story.id,
                          storyIdOf(story.chronicle, story.story),
                          path_);
                return CL_ERR_INVALID_CONF;
            }
            snapshot.stories.push_back(std::move(story));
        }
        if(rc != SQLITE_DONE)
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot read the stories of {}: {}", path_, sqlite3_errmsg(db_));
            return CL_ERR_INVALID_CONF;
        }
    }

    // In unsigned id order, as the in-memory store returns them.
    std::sort(snapshot.chronicles.begin(),
              snapshot.chronicles.end(),
              [](CatalogChronicle const& a, CatalogChronicle const& b) { return a.id < b.id; });
    std::sort(snapshot.stories.begin(),
              snapshot.stories.end(),
              [](CatalogStory const& a, CatalogStory const& b) { return a.id < b.id; });
    out = std::move(snapshot);
    return CL_SUCCESS;
}

int SqliteCatalogStore::insertChronicle(ChronicleId id, std::string const& name)
{
    if(!catalogIdsMatch(CatalogChronicle{id, name}))
    {
        LOG_ERROR("[SqliteCatalogStore] Refusing to insert chronicle '{}' with id {}: its name hashes to {}",
                  name,
                  id,
                  catalogChronicleIdOf(name));
        return CL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;
    Statement insert(db_, "INSERT INTO chronicles(id, name) VALUES (?1, ?2)");
    int const rc = insert.prepared() ? insert.integer(1, toSql(id)).text(2, name).step() : insert.rc();
    if(rc == SQLITE_DONE)
        return CL_SUCCESS;
    int const status = insertStatus(rc, CL_ERR_CHRONICLE_EXISTS);
    LOG_ERROR("[SqliteCatalogStore] Cannot insert chronicle '{}' (id {}): {}", name, id, sqlite3_errmsg(db_));
    return status;
}

int SqliteCatalogStore::insertStory(StoryId id,
                                    ChronicleId chronicle_id,
                                    std::string const& chronicle,
                                    std::string const& story)
{
    if(!catalogIdsMatch(chronicle_id, CatalogStory{id, chronicle, story}))
    {
        LOG_ERROR("[SqliteCatalogStore] Refusing to insert story '{}' of chronicle '{}' with id {} under chronicle "
                  "id {}: the names hash to {} and {}",
                  story,
                  chronicle,
                  id,
                  chronicle_id,
                  storyIdOf(chronicle, story),
                  catalogChronicleIdOf(chronicle));
        return CL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;
    Statement insert(db_, "INSERT INTO stories(id, chronicle_id, name) VALUES (?1, ?2, ?3)");
    int const rc = insert.prepared()
                           ? insert.integer(1, toSql(id)).integer(2, toSql(chronicle_id)).text(3, story).step()
                           : insert.rc();
    if(rc == SQLITE_DONE)
        return CL_SUCCESS;
    int const status = insertStatus(rc, CL_ERR_STORY_EXISTS);
    LOG_ERROR("[SqliteCatalogStore] Cannot insert story '{}' of chronicle '{}' (id {}): {}",
              story,
              chronicle,
              id,
              sqlite3_errmsg(db_));
    return status;
}

int SqliteCatalogStore::deleteStory(StoryId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;
    Statement remove(db_, "DELETE FROM stories WHERE id = ?1");
    int const rc = remove.prepared() ? remove.integer(1, toSql(id)).step() : remove.rc();
    if(rc != SQLITE_DONE)
    {
        LOG_ERROR("[SqliteCatalogStore] Cannot delete story {}: {}", id, sqlite3_errmsg(db_));
        return CL_ERR_UNKNOWN;
    }
    return sqlite3_changes(db_) != 0 ? CL_SUCCESS : CL_ERR_NOT_EXIST;
}

int SqliteCatalogStore::deleteChronicle(ChronicleId id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;
    Transaction txn(db_);
    if(!txn.begun())
        return CL_ERR_UNKNOWN;
    {
        Statement stories(db_, "DELETE FROM stories WHERE chronicle_id = ?1");
        if((stories.prepared() ? stories.integer(1, toSql(id)).step() : stories.rc()) != SQLITE_DONE)
        {
            LOG_ERROR("[SqliteCatalogStore] Cannot delete the stories of chronicle {}: {}", id, sqlite3_errmsg(db_));
            return CL_ERR_UNKNOWN;
        }
    }
    Statement chronicle(db_, "DELETE FROM chronicles WHERE id = ?1");
    if((chronicle.prepared() ? chronicle.integer(1, toSql(id)).step() : chronicle.rc()) != SQLITE_DONE)
    {
        LOG_ERROR("[SqliteCatalogStore] Cannot delete chronicle {}: {}", id, sqlite3_errmsg(db_));
        return CL_ERR_UNKNOWN;
    }
    // A chronicle that is not stored has no stories either, so the rollback
    // leaves the file as it was.
    if(sqlite3_changes(db_) == 0)
        return CL_ERR_NOT_EXIST;
    return txn.commit() ? CL_SUCCESS : CL_ERR_UNKNOWN;
}

int SqliteCatalogStore::setStoryDestroyPending(StoryId id, bool pending)
{
    return setDestroyPending("UPDATE stories SET destroy_pending = ?2 WHERE id = ?1", id, pending);
}

int SqliteCatalogStore::setChronicleDestroyPending(ChronicleId id, bool pending)
{
    return setDestroyPending("UPDATE chronicles SET destroy_pending = ?2 WHERE id = ?1", id, pending);
}

int SqliteCatalogStore::setDestroyPending(char const* sql, uint64_t id, bool pending)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return CL_ERR_UNKNOWN;
    Statement update(db_, sql);
    int const rc = update.prepared() ? update.integer(1, toSql(id)).integer(2, pending ? 1 : 0).step() : update.rc();
    if(rc != SQLITE_DONE)
    {
        LOG_ERROR("[SqliteCatalogStore] Cannot set destroy_pending={} on {}: {}", pending, id, sqlite3_errmsg(db_));
        return CL_ERR_UNKNOWN;
    }
    return sqlite3_changes(db_) != 0 ? CL_SUCCESS : CL_ERR_NOT_EXIST;
}

std::string SqliteCatalogStore::pragmaValue(std::string const& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(db_ == nullptr)
        return std::string();
    Statement pragma(db_, ("PRAGMA " + name).c_str());
    if(!pragma.prepared() || pragma.step() != SQLITE_ROW)
        return std::string();
    return pragma.columnText(0);
}

} // namespace chronolog
