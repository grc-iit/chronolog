#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <ClientConfiguration.h>

#include "chronopubsub_client_adapter.h"

namespace chronopubsub
{

static int DEFAULT_FLAGS = 0;

ChronoPubSubClientAdapter::ChronoPubSubClientAdapter(LogLevel level)
    : logLevel_(level)
{
    chronolog::ClientConfiguration client_config;
    initialize(client_config);
}

ChronoPubSubClientAdapter::ChronoPubSubClientAdapter(const std::string& config_path, LogLevel level)
    : logLevel_(level)
{
    chronolog::ClientConfiguration client_config;
    if(!config_path.empty())
    {
        CHRONOPUBSUB_INFO(logLevel_, "Loading ChronoLog client configuration from '", config_path, "'");
        if(!client_config.load_from_file(config_path))
        {
            CHRONOPUBSUB_ERROR(logLevel_, "Failed to load configuration file: ", config_path);
            throw std::runtime_error("Failed to load config file: " + config_path);
        }
    }
    initialize(client_config);
}

void ChronoPubSubClientAdapter::initialize(const chronolog::ClientConfiguration& client_config)
{
    chronolog::ClientPortalServiceConf portalConf{client_config.PORTAL_CONF.PROTO_CONF,
                                                  client_config.PORTAL_CONF.IP,
                                                  client_config.PORTAL_CONF.PORT,
                                                  client_config.PORTAL_CONF.PROVIDER_ID};

    chronolog::ClientQueryServiceConf queryConf{client_config.QUERY_CONF.PROTO_CONF,
                                                client_config.QUERY_CONF.IP,
                                                client_config.QUERY_CONF.PORT,
                                                client_config.QUERY_CONF.PROVIDER_ID};

    CHRONOPUBSUB_INFO(logLevel_, "Connecting to ChronoLog at ", portalConf.IP, ":", portalConf.PORT);
    chronolog = std::make_unique<chronolog::Client>(portalConf, queryConf);
    if(int ret = chronolog->Connect(); ret != chronolog::CL_SUCCESS)
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Connection failed with error code: ", ret);
        chronolog->Disconnect();
        chronolog.reset();
        throw std::runtime_error("Failed to connect to ChronoLog with error code: " + std::to_string(ret));
    }
    CHRONOPUBSUB_INFO(logLevel_, "Connected successfully");

    std::map<std::string, std::string> chronicle_attrs;
    if(int ret = chronolog->CreateChronicle(defaultChronicle, chronicle_attrs, DEFAULT_FLAGS);
       ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Failed to create chronicle '", defaultChronicle, "' with error code: ", ret);
        throw std::runtime_error("Failed to create chronicle with error code: " + std::to_string(ret));
    }
    CHRONOPUBSUB_INFO(logLevel_, "Chronicle '", defaultChronicle, "' ready for operations");
}

ChronoPubSubClientAdapter::~ChronoPubSubClientAdapter()
{
    if(chronolog)
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        CHRONOPUBSUB_INFO(logLevel_, "Releasing ", publishHandleCache.size(), " cached publish handles");
        for(const auto& [topic, handle]: publishHandleCache) { chronolog->ReleaseStory(defaultChronicle, topic); }
        publishHandleCache.clear();

        CHRONOPUBSUB_INFO(logLevel_, "Disconnecting from ChronoLog");
        chronolog->Disconnect();
    }
}

chronolog::StoryHandle* ChronoPubSubClientAdapter::getOrAcquirePublishHandle(const std::string& topic)
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    auto it = publishHandleCache.find(topic);
    if(it != publishHandleCache.end())
    {
        return it->second;
    }

    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, topic, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOPUBSUB_ERROR(logLevel_,
                           "Failed to acquire story handle for topic='",
                           topic,
                           "' with error code: ",
                           status);
        throw std::runtime_error("Failed to acquire story handle for topic: " + topic +
                                 ", error code: " + std::to_string(status));
    }

    publishHandleCache[topic] = handle;
    return handle;
}

void ChronoPubSubClientAdapter::flushCachedHandle(const std::string& topic)
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    auto it = publishHandleCache.find(topic);
    if(it != publishHandleCache.end())
    {
        chronolog->ReleaseStory(defaultChronicle, topic);
        publishHandleCache.erase(it);
    }
}

std::uint64_t ChronoPubSubClientAdapter::publishEvent(const std::string& topic, const std::string& payload)
{
    chronolog::StoryHandle* handle = getOrAcquirePublishHandle(topic);
    auto timestamp = handle->log_event(payload);
    CHRONOPUBSUB_DEBUG(logLevel_,
                       "Published event for topic='",
                       topic,
                       "' (payload_size=",
                       payload.size(),
                       ") timestamp=",
                       timestamp);
    return timestamp;
}

std::vector<Message>
ChronoPubSubClientAdapter::replayEvents(const std::string& topic, std::uint64_t start_ts, std::uint64_t end_ts)
{
    // Reads must not see a stale cached publish handle: ChronoLog requires the
    // handle to be released before events become replayable.
    flushCachedHandle(topic);

    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = chronolog->AcquireStory(defaultChronicle, topic, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOPUBSUB_ERROR(logLevel_,
                           "Failed to acquire story handle for topic='",
                           topic,
                           "' with error code: ",
                           status);
        throw std::runtime_error("Failed to acquire story handle for topic: " + topic +
                                 ", error code: " + std::to_string(status));
    }

    struct StoryHandleGuard
    {
        chronolog::Client* client;
        const std::string& chronicle;
        const std::string& topic;
        ~StoryHandleGuard() { client->ReleaseStory(chronicle, topic); }
    } guard{chronolog.get(), defaultChronicle, topic};

    std::vector<chronolog::Event> events;
    if(int ret = chronolog->ReplayStory(defaultChronicle, topic, start_ts, end_ts, events);
       ret != chronolog::CL_SUCCESS)
    {
        // Caller (subscriber polling loop) decides severity; throw with the code so
        // it can be retried without spamming ERROR for the propagation window.
        throw std::runtime_error("Failed to replay events for topic: " + topic +
                                 ", error code: " + std::to_string(ret));
    }

    std::vector<Message> messages;
    messages.reserve(events.size());
    for(const auto& event: events) { messages.emplace_back(event.time(), topic, event.log_record()); }

    CHRONOPUBSUB_DEBUG(logLevel_, "Replayed ", messages.size(), " events for topic='", topic, "'");
    return messages;
}

void ChronoPubSubClientAdapter::flush()
{
    std::lock_guard<std::mutex> lock(cacheMutex);

    if(publishHandleCache.empty())
    {
        return;
    }

    CHRONOPUBSUB_INFO(logLevel_, "Flushing ", publishHandleCache.size(), " cached publish handles");
    for(const auto& [topic, handle]: publishHandleCache) { chronolog->ReleaseStory(defaultChronicle, topic); }
    publishHandleCache.clear();
}

} // namespace chronopubsub
