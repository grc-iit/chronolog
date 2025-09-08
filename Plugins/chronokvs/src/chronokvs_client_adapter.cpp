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

    // Acquire story handle
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, defaultStory, story_attrs, flags);
    if (status != chronolog::CL_SUCCESS) {
        throw std::runtime_error("Failed to acquire story handle");
    }
    storyHandle = std::unique_ptr<chronolog::StoryHandle>(handle);
}

ChronoKVSClientAdapter::~ChronoKVSClientAdapter()
{
    if (storyHandle) {
        chronolog->ReleaseStory(defaultChronicle, defaultStory);
        storyHandle.reset();
    }
    if (chronolog) {
        chronolog->Disconnect();
    }
}

std::uint64_t ChronoKVSClientAdapter::storeEvent(const std::string &serializedEvent)
{
    if (!storyHandle) {
        throw std::runtime_error("Story handle is not initialized");
    }

    // Log the event using the story handle
    return storyHandle->log_event(serializedEvent);
}

std::vector<std::string> ChronoKVSClientAdapter::retrieveEvents(std::uint64_t timestamp)
{
    if (!storyHandle) {
        throw std::runtime_error("Story handle is not initialized");
    }

    // Replay events for the specific timestamp
    std::vector<chronolog::Event> events;
    int ret = chronolog->ReplayStory(defaultChronicle, defaultStory, timestamp, timestamp, events);
    if (ret != chronolog::CL_SUCCESS) {
        throw std::runtime_error("Failed to replay events");
    }

    // Extract event values
    std::vector<std::string> values;
    values.reserve(events.size());
    for (const auto& event : events) {
        values.push_back(event.value);
    }
    return values;
}

}