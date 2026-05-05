#ifndef CHRONOSQL_CLIENT_ADAPTER_H_
#define CHRONOSQL_CLIENT_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <chronolog_client.h>

namespace chronolog
{
class ClientConfiguration;
}

#include "chronosql_logger.h"

namespace chronosql
{

/**
 * @brief Thin wrapper over the ChronoLog Client that speaks in
 * timestamp/payload pairs and handles per-story handle caching.
 *
 * Mirrors ChronoKVS's adapter shape: writes go through cached handles;
 * reads release the cached write handle for that story before replay.
 */
class ChronoSQLClientAdapter
{
public:
    struct EventPayload
    {
        std::uint64_t timestamp;
        std::string payload;
    };

    ChronoSQLClientAdapter(const std::string& chronicle_name, LogLevel level);

    ChronoSQLClientAdapter(const std::string& chronicle_name, const std::string& config_path, LogLevel level);

    ~ChronoSQLClientAdapter();

    /// Append an event to the named story. Returns the assigned timestamp.
    std::uint64_t appendEvent(const std::string& story, const std::string& payload);

    /// Replay [start_ts, end_ts) from the named story.
    ///
    /// On a brand-new deployment a story may have no events committed yet, and
    /// the player can fail to mark such a query complete before the client-side
    /// replay timeout fires. When `tolerate_timeout` is true, a query timeout
    /// is logged as a warning and an empty vector is returned instead of
    /// throwing. Use this for read paths that legitimately tolerate "no data
    /// yet" (e.g. cold-start metadata replay).
    std::vector<EventPayload>
    replayEvents(const std::string& story, std::uint64_t start_ts, std::uint64_t end_ts, bool tolerate_timeout = false);

    /// Release all cached write handles. Required before reads if the same
    /// process holds active write handles for the target story.
    void flush();

    const std::string& chronicleName() const { return chronicle_; }

private:
    std::string chronicle_;
    LogLevel logLevel_;
    std::unique_ptr<chronolog::Client> client_;
    std::unordered_map<std::string, chronolog::StoryHandle*> handleCache_;
    mutable std::mutex cacheMutex_;

    void initialize(const chronolog::ClientConfiguration& client_config);
    chronolog::StoryHandle* getOrAcquireHandle(const std::string& story);
    void flushCachedHandle(const std::string& story);
};

} // namespace chronosql

#endif // CHRONOSQL_CLIENT_ADAPTER_H_
