#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>
#include "chronokvs_client_adapter.h"
#include <ClientConfiguration.h>
#include <chrono_monitor.h>
#include <client_errcode.h>

namespace chronokvs
{

// Default flags for ChronoLog operations - not const since the API requires non-const reference
static int DEFAULT_FLAGS = 0;

ChronoKVSClientAdapter::ChronoKVSClientAdapter()
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
    LOG_INFO("[ChronoKVS] Connecting to ChronoLog at {}:{}", portalConf.IP, portalConf.PORT);
    chronolog = std::make_unique<chronolog::Client>(portalConf, queryConf);
    if(int ret = chronolog->Connect(); ret != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoKVS] Connection failed with error code: {}",
                  chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret)));
        chronolog->Disconnect();
        chronolog.reset();
        throw std::runtime_error("Failed to connect to ChronoLog: " +
                                 std::string(chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret))));
    }
    LOG_INFO("[ChronoKVS] Connected successfully");

    // Ensure the default chronicle exists
    std::map<std::string, std::string> chronicle_attrs;
    if(int ret = chronolog->CreateChronicle(defaultChronicle, chronicle_attrs, DEFAULT_FLAGS);
       ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        LOG_ERROR("[ChronoKVS] Failed to create chronicle '{}' with error code: {}",
                  defaultChronicle, chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret)));
        throw std::runtime_error("Failed to create chronicle: " +
                                 std::string(chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret))));
    }
    LOG_DEBUG("[ChronoKVS] Chronicle '{}' ready for operations", defaultChronicle);
}

ChronoKVSClientAdapter::~ChronoKVSClientAdapter()
{
    if(chronolog)
    {
        LOG_DEBUG("[ChronoKVS] Disconnecting from ChronoLog");
        chronolog->Disconnect();
    }
}

std::uint64_t ChronoKVSClientAdapter::storeEvent(const std::string& key, const std::string& value)
{
    LOG_DEBUG("[ChronoKVS] Storing event for key='{}' (value_size={})", key, value.size());

    // Acquire a story handle for the given key
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoKVS] Failed to acquire story handle for key='{}' with error code: {}",
                  key, chronolog::to_string(static_cast<chronolog::ClientErrorCode>(status)));
        throw std::runtime_error("Failed to acquire story handle for key: " + key +
                                 ", error: " + std::string(chronolog::to_string(static_cast<chronolog::ClientErrorCode>(status))));
    }

    // RAII-style story handle management using scope guard
    struct StoryHandleGuard
    {
        chronolog::Client* client;
        const std::string& chronicle;
        const std::string& key;
        ~StoryHandleGuard() { client->ReleaseStory(chronicle, key); }
    } guard{chronolog.get(), defaultChronicle, key};

    auto timestamp = handle->log_event(value);
    LOG_DEBUG("[ChronoKVS] Event stored successfully for key='{}' with timestamp={}", key, timestamp);
    return timestamp;
}

std::vector<EventData>
ChronoKVSClientAdapter::retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts)
{
    LOG_DEBUG("[ChronoKVS] Retrieving events for key='{}' range=[{}, {})", key, start_ts, end_ts);

    // Acquire a story handle for the given key
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ChronoKVS] Failed to acquire story handle for key='{}' with error code: {}",
                  key, chronolog::to_string(static_cast<chronolog::ClientErrorCode>(status)));
        throw std::runtime_error("Failed to acquire story handle for key: " + key +
                                 ", error: " + std::string(chronolog::to_string(static_cast<chronolog::ClientErrorCode>(status))));
    }

    // RAII-style story handle management
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
        LOG_ERROR("[ChronoKVS] Failed to replay events for key='{}' with error code: {}",
                  key, chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret)));
        throw std::runtime_error("Failed to replay events for key: " + key +
                                 ", error: " + std::string(chronolog::to_string(static_cast<chronolog::ClientErrorCode>(ret))));
    }

    // Transform ChronoLog events into EventData objects
    std::vector<EventData> eventDataList;
    eventDataList.reserve(events.size());
    for(const auto& event: events) { eventDataList.emplace_back(event.time(), event.log_record()); }

    LOG_DEBUG("[ChronoKVS] Retrieved {} events for key='{}'", eventDataList.size(), key);
    return eventDataList;
}

} // namespace chronokvs