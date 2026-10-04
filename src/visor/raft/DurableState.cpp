#include "visor/raft/DurableState.h"
#include <cstring>
#include <stdexcept>
namespace chronolog::visor
{
using namespace nuraft;
namespace
{
struct Query
{
    sqlite3_stmt* stmt{};
    Query(sqlite3* db, const std::string& sql)
    {
        if(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query() { sqlite3_finalize(stmt); }
};
ptr<buffer> blob(sqlite3_stmt* stmt)
{
    size_t size = static_cast<size_t>(sqlite3_column_bytes(stmt, 0));
    auto out = buffer::alloc(size);
    if(size)
        std::memcpy(out->data_begin(), sqlite3_column_blob(stmt, 0), size);
    return out;
}
} // namespace
DurableState::DurableState(const std::string& path, RaftConfig config)
    : config_(std::move(config))
{
    if(sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        throw std::runtime_error("cannot open raft database");
    sqlite3_busy_timeout(db_, 5000);
    sql("PRAGMA journal_mode=WAL");
    sql("PRAGMA synchronous=FULL");
    sql("CREATE TABLE IF NOT EXISTS logs(idx INTEGER PRIMARY KEY, value BLOB NOT NULL)");
    sql("CREATE TABLE IF NOT EXISTS state(key TEXT PRIMARY KEY, value BLOB NOT NULL)");
    sql("CREATE TABLE IF NOT EXISTS bounds(start INTEGER NOT NULL)");
    sql("INSERT INTO bounds SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM bounds)");
}
DurableState::~DurableState() { sqlite3_close(db_); }
std::string DurableState::pragmaValue(const std::string& name) const
{
    std::lock_guard lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if(sqlite3_prepare_v2(db_, ("PRAGMA " + name).c_str(), -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
    std::string value;
    if(sqlite3_step(statement) == SQLITE_ROW)
        if(const auto* text = sqlite3_column_text(statement, 0))
            value = reinterpret_cast<const char*>(text);
    sqlite3_finalize(statement);
    return value;
}
void DurableState::sql(const std::string& query) const
{
    if(sqlite3_exec(db_, query.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
}
uint64_t DurableState::scalar(const std::string& query) const
{
    Query q(db_, query);
    if(sqlite3_step(q.stmt) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db_));
    return static_cast<uint64_t>(sqlite3_column_int64(q.stmt, 0));
}
ulong DurableState::next_slot() const
{
    std::lock_guard lock(mutex_);
    return scalar("SELECT MAX((SELECT start FROM bounds), COALESCE(MAX(idx)+1,1)) FROM logs");
}
ulong DurableState::start_index() const
{
    std::lock_guard lock(mutex_);
    return scalar("SELECT start FROM bounds");
}
ptr<log_entry> DurableState::last_entry() const
{
    std::lock_guard lock(mutex_);
    auto index = next_slot() - 1;
    if(index < start_index())
        return cs_new<log_entry>(0, buffer::alloc(0));
    return const_cast<DurableState*>(this)->entry_at(index);
}
void DurableState::insert(uint64_t index, const buffer& value)
{
    Query q(db_, "INSERT OR REPLACE INTO logs VALUES(?1,?2)");
    sqlite3_bind_int64(q.stmt, 1, static_cast<sqlite3_int64>(index));
    sqlite3_bind_blob64(q.stmt, 2, value.data_begin(), value.size(), SQLITE_TRANSIENT);
    if(sqlite3_step(q.stmt) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
}
ulong DurableState::append(ptr<log_entry>& entry)
{
    std::lock_guard lock(mutex_);
    auto index = next_slot();
    insert(index, *entry->serialize());
    return index;
}
void DurableState::write_at(ulong index, ptr<log_entry>& entry)
{
    std::lock_guard lock(mutex_);
    sql("BEGIN IMMEDIATE");
    try
    {
        sql("DELETE FROM logs WHERE idx >= " + std::to_string(index));
        insert(index, *entry->serialize());
        sql("COMMIT");
    }
    catch(...)
    {
        sql("ROLLBACK");
        throw;
    }
}
ptr<log_entry> DurableState::entry_at(ulong index)
{
    std::lock_guard lock(mutex_);
    Query q(db_, "SELECT value FROM logs WHERE idx=" + std::to_string(index));
    if(sqlite3_step(q.stmt) != SQLITE_ROW)
        return nullptr;
    auto b = blob(q.stmt);
    return log_entry::deserialize(*b);
}
ulong DurableState::term_at(ulong index)
{
    auto e = entry_at(index);
    return e ? e->get_term() : 0;
}
ptr<std::vector<ptr<log_entry>>> DurableState::log_entries(ulong start, ulong end)
{
    std::lock_guard lock(mutex_);
    auto out = cs_new<std::vector<ptr<log_entry>>>();
    for(auto i = start; i < end; ++i)
    {
        auto e = entry_at(i);
        if(!e)
            return nullptr;
        out->push_back(e);
    }
    return out;
}
ptr<buffer> DurableState::pack(ulong index, int32_t count)
{
    std::lock_guard lock(mutex_);
    std::vector<ptr<buffer>> entries;
    size_t size = sizeof(int32_t);
    for(int32_t i = 0; i < count; ++i)
    {
        auto e = entry_at(index + static_cast<ulong>(i));
        if(!e)
            throw std::runtime_error("missing packed log");
        auto b = e->serialize();
        size += sizeof(int32_t) + b->size();
        entries.push_back(b);
    }
    auto out = buffer::alloc(size);
    out->put(count);
    for(auto& b: entries)
    {
        out->put(static_cast<int32_t>(b->size()));
        out->put(*b);
    }
    out->pos(0);
    return out;
}
void DurableState::apply_pack(ulong index, buffer& data)
{
    std::lock_guard lock(mutex_);
    data.pos(0);
    auto count = data.get_int();
    sql("BEGIN IMMEDIATE");
    try
    {
        sql("DELETE FROM logs WHERE idx >= " + std::to_string(index));
        for(int32_t i = 0; i < count; ++i)
        {
            auto size = data.get_int();
            if(size < 0 || static_cast<size_t>(size) > data.size() - data.pos())
                throw std::runtime_error("invalid log pack");
            auto b = buffer::alloc(static_cast<size_t>(size));
            data.get(b);
            insert(index + static_cast<ulong>(i), *b);
        }
        sql("COMMIT");
    }
    catch(...)
    {
        sql("ROLLBACK");
        throw;
    }
}
bool DurableState::compact(ulong index)
{
    std::lock_guard lock(mutex_);
    sql("BEGIN IMMEDIATE");
    try
    {
        sql("DELETE FROM logs WHERE idx <= " + std::to_string(index));
        sql("UPDATE bounds SET start=MAX(start," + std::to_string(index + 1) + ")");
        sql("COMMIT");
    }
    catch(...)
    {
        sql("ROLLBACK");
        throw;
    }
    return true;
}
bool DurableState::flush()
{
    std::lock_guard lock(mutex_);
    return sqlite3_db_cacheflush(db_) == SQLITE_OK;
}
ptr<buffer> DurableState::get(const std::string& key) const
{
    std::lock_guard lock(mutex_);
    Query q(db_, "SELECT value FROM state WHERE key=?1");
    sqlite3_bind_text(q.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(q.stmt) == SQLITE_ROW ? blob(q.stmt) : nullptr;
}
void DurableState::put(const std::string& key, const buffer& value)
{
    std::lock_guard lock(mutex_);
    Query q(db_, "INSERT OR REPLACE INTO state VALUES(?1,?2)");
    sqlite3_bind_text(q.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob64(q.stmt, 2, value.data_begin(), value.size(), SQLITE_TRANSIENT);
    if(sqlite3_step(q.stmt) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
}
ptr<cluster_config> DurableState::load_config()
{
    if(auto b = get("config"))
        return cluster_config::deserialize(*b);
    auto c = cs_new<cluster_config>();
    for(const auto& p: config_.peers) c->get_servers().push_back(cs_new<srv_config>(p.id, p.raft_endpoint));
    return c;
}
void DurableState::save_config(const cluster_config& config) { put("config", *config.serialize()); }
void DurableState::save_state(const srv_state& state) { put("state", *state.serialize()); }
ptr<srv_state> DurableState::read_state()
{
    auto b = get("state");
    return b ? srv_state::deserialize(*b) : nullptr;
}
ptr<log_store> DurableState::load_log_store()
{
    return ptr<log_store>(this, [](log_store*) {});
}
int32_t DurableState::server_id() { return config_.server_id; }
void DurableState::system_exit(int) { std::terminate(); }
} // namespace chronolog::visor
