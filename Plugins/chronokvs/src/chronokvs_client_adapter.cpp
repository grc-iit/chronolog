#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>
#include "chronokvs_client_adapter.h"
#include <ClientConfiguration.h>

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
    chronolog = std::make_unique<chronolog::Client>(portalConf, queryConf);
    if(int ret = chronolog->Connect(); ret != chronolog::CL_SUCCESS)
    {
        chronolog->Disconnect();
        chronolog.reset();
        throw std::runtime_error("Failed to connect to ChronoLog");
    }

    // Ensure the default chronicle exists
    std::map<std::string, std::string> chronicle_attrs;
    if(int ret = chronolog->CreateChronicle(defaultChronicle, chronicle_attrs, DEFAULT_FLAGS);
       ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        throw std::runtime_error("Failed to create chronicle");
    }
}

ChronoKVSClientAdapter::~ChronoKVSClientAdapter()
{
    if(chronolog)
    {
        chronolog->Disconnect();
    }
}

std::uint64_t ChronoKVSClientAdapter::storeEvent(const std::string& key, const std::string& value)
{
    // Acquire a story handle for the given key
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        throw std::runtime_error("Failed to acquire story handle for key: " + key);
    }

    // RAII-style story handle management using scope guard
    struct StoryHandleGuard
    {
        chronolog::Client* client;
        const std::string& chronicle;
        const std::string& key;
        ~StoryHandleGuard() { client->ReleaseStory(chronicle, key); }
    } guard{chronolog.get(), defaultChronicle, key};

    return handle->log_event(value);
}

std::vector<EventData>
ChronoKVSClientAdapter::retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts)
{
    // Acquire a story handle for the given key
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        throw std::runtime_error("Failed to acquire story handle for key: " + key);
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
        throw std::runtime_error("Failed to replay events for key: " + key);
    }

    // Transform ChronoLog events into EventData objects
    std::vector<EventData> eventDataList;
    eventDataList.reserve(events.size());
    for(const auto& event: events) { eventDataList.emplace_back(event.time(), event.log_record()); }
    return eventDataList;
}

} // namespace chronokvs