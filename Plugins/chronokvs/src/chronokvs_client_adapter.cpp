#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>

#include <chimaera/chimaera.h>

#include "chronokvs_client_adapter.h"


namespace chronokvs
{

ChronoKVSClientAdapter::ChronoKVSClientAdapter(LogLevel level,
                                               const chi::PoolId& keeper_pool_id,
                                               const chi::PoolId& player_pool_id)
    : logLevel_(level)
{
    CHRONOKVS_INFO(logLevel_, "Initializing ChronoLog client wrapper");
    chronolog = std::make_unique<chronolog::Client>(keeper_pool_id, player_pool_id);

    // Ensure the default chronicle exists
    if(int ret = chronolog->CreateChronicle(defaultChronicle);
       ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        CHRONOKVS_ERROR(logLevel_, "Failed to create chronicle '", defaultChronicle, "' with error code: ", ret);
        throw std::runtime_error("Failed to create chronicle with error code: " + std::to_string(ret));
    }
    CHRONOKVS_INFO(logLevel_, "Chronicle '", defaultChronicle, "' ready for operations");
}

ChronoKVSClientAdapter::~ChronoKVSClientAdapter()
{
    if(chronolog)
    {
        // Release all cached story handles before destroying client
        std::lock_guard<std::mutex> lock(cacheMutex);
        CHRONOKVS_INFO(logLevel_, "Releasing ", handleCache.size(), " cached story handles");
        for(const auto& [key, handle]: handleCache)
        {
            CHRONOKVS_DEBUG(logLevel_, "Releasing cached handle for key='", key, "'");
            chronolog->ReleaseStory(defaultChronicle, key);
        }
        handleCache.clear();
    }
}

chronolog::StoryHandle* ChronoKVSClientAdapter::getOrAcquireHandle(const std::string& key)
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    // Check if handle is already cached
    auto it = handleCache.find(key);
    if(it != handleCache.end())
    {
        CHRONOKVS_DEBUG(logLevel_, "Cache hit for key='", key, "'");
        return it->second;
    }

    // Cache miss - acquire new handle
    CHRONOKVS_DEBUG(logLevel_, "Cache miss for key='", key, "', acquiring new handle");
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOKVS_ERROR(logLevel_, "Failed to acquire story handle for key='", key, "' with error code: ", status);
        throw std::runtime_error("Failed to acquire story handle for key: " + key +
                                 ", error code: " + std::to_string(status));
    }

    // Store in cache and return
    handleCache[key] = handle;
    CHRONOKVS_DEBUG(logLevel_, "Cached new handle for key='", key, "' (cache size: ", handleCache.size(), ")");
    return handle;
}

void ChronoKVSClientAdapter::flushCachedHandle(const std::string& key)
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    auto it = handleCache.find(key);
    if(it != handleCache.end())
    {
        CHRONOKVS_DEBUG(logLevel_, "Flushing cached handle for key='", key, "' before read operation");
        chronolog->ReleaseStory(defaultChronicle, key);
        handleCache.erase(it);
    }
}

std::uint64_t ChronoKVSClientAdapter::storeEvent(const std::string& key, const std::string& value)
{
    CHRONOKVS_DEBUG(logLevel_, "Storing event for key='", key, "' (value_size=", value.size(), ")");

    // Get cached handle or acquire a new one
    chronolog::StoryHandle* handle = getOrAcquireHandle(key);

    auto timestamp = handle->log_event(value);
    CHRONOKVS_DEBUG(logLevel_, "Event stored successfully for key='", key, "' with timestamp=", timestamp);
    return timestamp;
}

std::vector<EventData>
ChronoKVSClientAdapter::retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts)
{
    CHRONOKVS_DEBUG(logLevel_, "Retrieving events for key='", key, "' range=[", start_ts, ", ", end_ts, ")");

    // Flush any cached write handle for this key to ensure data is committed and visible
    flushCachedHandle(key);

    // Acquire a fresh handle for the read operation
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOKVS_ERROR(logLevel_, "Failed to acquire story handle for key='", key, "' with error code: ", status);
        throw std::runtime_error("Failed to acquire story handle for key: " + key +
                                 ", error code: " + std::to_string(status));
    }

    // RAII-style story handle management for reads
    struct StoryHandleGuard
    {
        chronolog::Client* client;
        const std::string& chronicle;
        const std::string& key;
        ~StoryHandleGuard() { client->ReleaseStory(chronicle, key); }
    } guard{chronolog.get(), defaultChronicle, key};

    // Retrieve events for the specified time range
    std::vector<chronolog::Event> events;
    if(int ret = chronolog->ReplayStory(defaultChronicle, key, start_ts, end_ts, events); ret != chronolog::CL_SUCCESS)
    {
        CHRONOKVS_ERROR(logLevel_, "Failed to replay events for key='", key, "' with error code: ", ret);
        throw std::runtime_error("Failed to replay events for key: " + key + ", error code: " + std::to_string(ret));
    }

    // Transform ChronoLog events into EventData objects
    std::vector<EventData> eventDataList;
    eventDataList.reserve(events.size());
    for(const auto& event: events) { eventDataList.emplace_back(event.time(), event.log_record()); }

    CHRONOKVS_DEBUG(logLevel_, "Retrieved ", eventDataList.size(), " events for key='", key, "'");
    return eventDataList;
}

void ChronoKVSClientAdapter::flush()
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    if(handleCache.empty())
    {
        CHRONOKVS_DEBUG(logLevel_, "Flush called but no cached handles to release");
        return;
    }

    CHRONOKVS_INFO(logLevel_, "Flushing ", handleCache.size(), " cached story handles");
    for(const auto& [key, handle]: handleCache)
    {
        CHRONOKVS_DEBUG(logLevel_, "Releasing cached handle for key='", key, "'");
        chronolog->ReleaseStory(defaultChronicle, key);
    }
    handleCache.clear();
    CHRONOKVS_INFO(logLevel_, "All cached handles released - data committed for propagation");
}

} // namespace chronokvs
