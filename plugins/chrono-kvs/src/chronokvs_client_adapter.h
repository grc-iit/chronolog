#ifndef CHRONOKVS_CLIENT_ADAPTER_H_
#define CHRONOKVS_CLIENT_ADAPTER_H_

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include <mutex>

#include <chronolog_client.h>

namespace chronolog
{
class ClientConfiguration;
}

#include "chronokvs_types.h"
#include "chronokvs_logger.h"

namespace chronokvs
{

class ChronoKVSClientAdapter
{
private:
    std::unique_ptr<chronolog::Client> chronolog;
    const std::string defaultChronicle = "ChronoKVSChronicle";
    LogLevel logLevel_;

    // Story handle cache: maps key names to their acquired handles
    // Handles are cached to avoid repeated acquire/release cycles for writes
    std::unordered_map<std::string, chronolog::StoryHandle*> handleCache;

    // Mutex to protect the handle cache in multi-threaded contexts
    mutable std::mutex cacheMutex;

    // Helper method to get a cached handle or acquire a new one (for writes)
    chronolog::StoryHandle* getOrAcquireHandle(const std::string& key);

    // Helper method to release and remove a cached handle for a key (before reads)
    void flushCachedHandle(const std::string& key);

    // Connect to ChronoLog using the already-populated configuration and ensure
    // the default chronicle exists. Used by all constructors.
    void initialize(const chronolog::ClientConfiguration& client_config);

public:
    explicit ChronoKVSClientAdapter(LogLevel level);

    explicit ChronoKVSClientAdapter(const std::string& config_path, LogLevel level);

    ~ChronoKVSClientAdapter();

    std::uint64_t storeEvent(const std::string& key, const std::string& value);

    std::vector<EventData> retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts);

    // Flush all cached handles to ensure data is committed
    // Call this before waiting for data propagation if using cached writes
    void flush();
};
} // namespace chronokvs
#endif // CHRONOKVS_CLIENT_ADAPTER_H_
