#ifndef CHRONOLOG_SQLITE_CATALOG_STORE_H
#define CHRONOLOG_SQLITE_CATALOG_STORE_H

#include <mutex>
#include <string>

#include "VisorCatalogStore.h"

struct sqlite3;

namespace chronolog
{

// The visor's catalog in one SQLite database on a disk local to the visor's
// node. One connection, opened by open() and used under one mutex. The
// connection holds an exclusive lock on the file, so a second store on the
// same file fails to open it.
class SqliteCatalogStore: public VisorCatalogStore
{
public:
    static constexpr int SCHEMA_VERSION = 1;
    static constexpr int DEFAULT_BUSY_TIMEOUT_MS = 5000;

    // busy_timeout_ms: how long a statement waits for a lock held elsewhere.
    explicit SqliteCatalogStore(std::string path, int busy_timeout_ms = DEFAULT_BUSY_TIMEOUT_MS);
    ~SqliteCatalogStore() override;

    SqliteCatalogStore(SqliteCatalogStore const&) = delete;
    SqliteCatalogStore& operator=(SqliteCatalogStore const&) = delete;

    int open() override;
    // Closes the connection, which releases the file's lock. Later calls fail
    // with CL_ERR_UNKNOWN until the next open().
    void close();
    int load(CatalogSnapshot& out) override;
    int insertChronicle(ChronicleId, std::string const& name) override;
    int insertStory(StoryId, ChronicleId, std::string const& chronicle, std::string const& story) override;
    int deleteStory(StoryId) override;
    int deleteChronicle(ChronicleId) override;
    int setStoryDestroyPending(StoryId, bool) override;
    int setChronicleDestroyPending(ChronicleId, bool) override;

    // Runs "PRAGMA <name>" on the store's connection and returns its first
    // column as text, or an empty string on error. For tests and logging.
    std::string pragmaValue(std::string const& name);

private:
    int openLocked();
    int setDestroyPending(char const* sql, uint64_t id, bool pending);

    std::string path_;
    int busyTimeoutMs_;
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

} // namespace chronolog

#endif // CHRONOLOG_SQLITE_CATALOG_STORE_H
