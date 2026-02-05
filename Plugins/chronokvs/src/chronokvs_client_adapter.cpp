#include <cstdint>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>
#include <ClientConfiguration.h>

#include "chronokvs_client_adapter.h"


namespace chronokvs
{

// Default flags for ChronoLog operations - not const since the API requires non-const reference
static int DEFAULT_FLAGS = 0;

ChronoKVSClientAdapter::ChronoKVSClientAdapter(LogLevel level)
    : logLevel_(level)
{
    chronolog::ClientConfiguration confManager;

    // Configure portal and query services from the configuration manager
    chronolog::ClientPortalServiceConf portalConf{confManager.PORTAL_CONF.PROTO_CONF,
                                                  confManager.PORTAL_CONF.IP,
                                                  confManager.PORTAL_CONF.PORT,
                                                  confManager.PORTAL_CONF.PROVIDER_ID};

    chronolog::ClientQueryServiceConf queryConf{confManager.QUERY_CONF.PROTO_CONF,
                                                confManager.QUERY_CONF.IP,
                                                confManager.QUERY_CONF.PORT,
                                                confManager.QUERY_CONF.PROVIDER_ID};

    // Initialize and connect the ChronoLog client
    CHRONOKVS_INFO(logLevel_, "Connecting to ChronoLog at ", portalConf.IP, ":", portalConf.PORT);
    chronolog = std::make_unique<chronolog::Client>(portalConf, queryConf);
    if(int ret = chronolog->Connect(); ret != chronolog::CL_SUCCESS)
    {
        CHRONOKVS_ERROR(logLevel_, "Connection failed with error code: ", ret);
        chronolog->Disconnect();
        chronolog.reset();
        throw std::runtime_error("Failed to connect to ChronoLog with error code: " + std::to_string(ret));
    }
    CHRONOKVS_INFO(logLevel_, "Connected successfully");

    // Ensure the default chronicle exists
    std::map<std::string, std::string> chronicle_attrs;
    if(int ret = chronolog->CreateChronicle(defaultChronicle, chronicle_attrs, DEFAULT_FLAGS);
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
        // Release all cached story handles before disconnecting
        std::lock_guard<std::mutex> lock(cacheMutex);
        CHRONOKVS_INFO(logLevel_, "Releasing ", handleCache.size(), " cached story handles");
        for(const auto& [key, handle]: handleCache)
        {
            CHRONOKVS_DEBUG(logLevel_, "Releasing cached handle for key='", key, "'");
            chronolog->ReleaseStory(defaultChronicle, key);
        }
        handleCache.clear();

        CHRONOKVS_INFO(logLevel_, "Disconnecting from ChronoLog");
        chronolog->Disconnect();
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
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, DEFAULT_FLAGS);
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

    // Ensure handle is acquired/cached (needed for ReplayStory to work)
    getOrAcquireHandle(key);

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

} // namespace chronokvs