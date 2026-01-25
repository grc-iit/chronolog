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
#include "chronokvs_types.h"

namespace chronokvs
{

class ChronoKVSClientAdapter
{
private:
    std::unique_ptr<chronolog::Client> chronolog;
    const std::string defaultChronicle = "ChronoKVSChronicle";

    // Story handle cache: maps key names to their acquired handles
    // Handles are cached to avoid repeated acquire/release cycles for the same key
    std::unordered_map<std::string, chronolog::StoryHandle*> handleCache;

    // Mutex to protect the handle cache in multi-threaded contexts
    mutable std::mutex cacheMutex;

    // Helper method to get a cached handle or acquire a new one
    chronolog::StoryHandle* getOrAcquireHandle(const std::string& key);

public:
    ChronoKVSClientAdapter();

    ~ChronoKVSClientAdapter();

    std::uint64_t storeEvent(const std::string& key, const std::string& value);

    std::vector<EventData> retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts);
};
} // namespace chronokvs
#endif // CHRONOKVS_CLIENT_ADAPTER_H_