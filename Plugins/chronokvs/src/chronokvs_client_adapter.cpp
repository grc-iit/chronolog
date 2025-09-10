#include "chronokvs_client_adapter.h"
#include <ClientConfiguration.h>

namespace chronokvs
{

ChronoKVSClientAdapter::ChronoKVSClientAdapter()
{
    // Initialize client configuration
    chronolog::ClientConfiguration confManager;
    
    // Build portal configs
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    // Build query configs
    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP = confManager.QUERY_CONF.IP;
    queryConf.PORT = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    // Create and connect client
    chronolog = std::make_unique<chronolog::Client>(portalConf, queryConf);
    int ret = chronolog->Connect();
    if (ret != chronolog::CL_SUCCESS) {
        chronolog->Disconnect();
        chronolog.reset();
        throw std::runtime_error("Failed to connect to ChronoLog");
    }

    // Create chronicle if it doesn't exist
    std::map<std::string, std::string> chronicle_attrs;
    int flags = 0;
    ret = chronolog->CreateChronicle(defaultChronicle, chronicle_attrs, flags);
    if (ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS) {
        throw std::runtime_error("Failed to create chronicle");
    }

    // No story handles are acquired in the constructor anymore
    // They will be acquired on-demand when storing or retrieving events
}

ChronoKVSClientAdapter::~ChronoKVSClientAdapter()
{
    if (chronolog) {
        chronolog->Disconnect();
    }
}

std::uint64_t ChronoKVSClientAdapter::storeEvent(const std::string &key, const std::string &value)
{
    std::map<std::string, std::string> story_attrs;
    int flags = 0;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, flags);
    if (status != chronolog::CL_SUCCESS) {
        throw std::runtime_error("Failed to acquire story handle for key: " + key);
    }

    // Log the event using the story handle
    auto timestamp = handle->log_event(value);
    
    // Release the story handle
    chronolog->ReleaseStory(defaultChronicle, key);
    
    return timestamp;
}

std::vector<EventData> ChronoKVSClientAdapter::retrieveEvents(const std::string &key, std::uint64_t start_ts, std::uint64_t end_ts)
{
    std::map<std::string, std::string> story_attrs;
    int flags = 0;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, key, story_attrs, flags);
    if (status != chronolog::CL_SUCCESS) {
        throw std::runtime_error("Failed to acquire story handle for key: " + key);
    }

    // Replay events for the given timestamp range
    std::vector<chronolog::Event> events;
    int ret = chronolog->ReplayStory(defaultChronicle, key, start_ts, end_ts, events);
    
    // Release the story handle
    chronolog->ReleaseStory(defaultChronicle, key);
    
    if (ret != chronolog::CL_SUCCESS) {
        throw std::runtime_error("Failed to replay events for key: " + key);
    }

    // Extract event data
    std::vector<EventData> eventDataList;
    eventDataList.reserve(events.size());
    for (const auto& event : events) {
        eventDataList.emplace_back(event.time(), event.log_record());
    }
    return eventDataList;
}

}