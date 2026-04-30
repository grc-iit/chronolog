#include "chronosql_client_adapter.h"

#include <map>
#include <stdexcept>

#include <ClientConfiguration.h>

namespace chronosql
{

namespace
{
int DEFAULT_FLAGS = 0;
}

ChronoSQLClientAdapter::ChronoSQLClientAdapter(const std::string& chronicle_name, LogLevel level)
    : chronicle_(chronicle_name)
    , logLevel_(level)
{
    chronolog::ClientConfiguration confManager;
    initialize(confManager);
}

ChronoSQLClientAdapter::ChronoSQLClientAdapter(const std::string& chronicle_name,
                                               const std::string& config_path,
                                               LogLevel level)
    : chronicle_(chronicle_name)
    , logLevel_(level)
{
    chronolog::ClientConfiguration confManager;
    if(!config_path.empty())
    {
        CHRONOSQL_INFO(logLevel_, "Loading ChronoLog client configuration from", config_path);
        if(!confManager.load_from_file(config_path))
        {
            CHRONOSQL_ERROR(logLevel_, "Failed to load configuration file:", config_path);
            throw std::runtime_error("Failed to load config file: " + config_path);
        }
    }
    initialize(confManager);
}

void ChronoSQLClientAdapter::initialize(const chronolog::ClientConfiguration& confManager)
{
    chronolog::ClientPortalServiceConf portalConf{confManager.PORTAL_CONF.PROTO_CONF,
                                                  confManager.PORTAL_CONF.IP,
                                                  confManager.PORTAL_CONF.PORT,
                                                  confManager.PORTAL_CONF.PROVIDER_ID};
    chronolog::ClientQueryServiceConf queryConf{confManager.QUERY_CONF.PROTO_CONF,
                                                confManager.QUERY_CONF.IP,
                                                confManager.QUERY_CONF.PORT,
                                                confManager.QUERY_CONF.PROVIDER_ID};

    CHRONOSQL_INFO(logLevel_, "Connecting to ChronoLog at", portalConf.IP, ":", portalConf.PORT);
    client_ = std::make_unique<chronolog::Client>(portalConf, queryConf);
    if(int ret = client_->Connect(); ret != chronolog::CL_SUCCESS)
    {
        CHRONOSQL_ERROR(logLevel_, "Connection failed with error code:", ret);
        client_->Disconnect();
        client_.reset();
        throw std::runtime_error("Failed to connect to ChronoLog with error code: " + std::to_string(ret));
    }

    std::map<std::string, std::string> chronicle_attrs;
    if(int ret = client_->CreateChronicle(chronicle_, chronicle_attrs, DEFAULT_FLAGS);
       ret != chronolog::CL_SUCCESS && ret != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        CHRONOSQL_ERROR(logLevel_, "Failed to create chronicle '", chronicle_, "' with error code:", ret);
        throw std::runtime_error("Failed to create chronicle with error code: " + std::to_string(ret));
    }
    CHRONOSQL_INFO(logLevel_, "Chronicle '", chronicle_, "' ready for operations");
}

ChronoSQLClientAdapter::~ChronoSQLClientAdapter()
{
    if(!client_)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for(const auto& [story, handle]: handleCache_) { client_->ReleaseStory(chronicle_, story); }
    handleCache_.clear();
    client_->Disconnect();
}

chronolog::StoryHandle* ChronoSQLClientAdapter::getOrAcquireHandle(const std::string& story)
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = handleCache_.find(story);
    if(it != handleCache_.end())
    {
        return it->second;
    }
    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = client_->AcquireStory(chronicle_, story, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOSQL_ERROR(logLevel_, "Failed to acquire story handle for story='", story, "' code:", status);
        throw std::runtime_error("Failed to acquire story handle for story: " + story +
                                 ", error code: " + std::to_string(status));
    }
    handleCache_[story] = handle;
    return handle;
}

void ChronoSQLClientAdapter::flushCachedHandle(const std::string& story)
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = handleCache_.find(story);
    if(it != handleCache_.end())
    {
        client_->ReleaseStory(chronicle_, story);
        handleCache_.erase(it);
    }
}

std::uint64_t ChronoSQLClientAdapter::appendEvent(const std::string& story, const std::string& payload)
{
    chronolog::StoryHandle* handle = getOrAcquireHandle(story);
    return handle->log_event(payload);
}

std::vector<ChronoSQLClientAdapter::EventPayload>
ChronoSQLClientAdapter::replayEvents(const std::string& story, std::uint64_t start_ts, std::uint64_t end_ts)
{
    flushCachedHandle(story);

    std::map<std::string, std::string> story_attrs;
    auto [status, handle] = client_->AcquireStory(chronicle_, story, story_attrs, DEFAULT_FLAGS);
    if(status != chronolog::CL_SUCCESS)
    {
        CHRONOSQL_ERROR(logLevel_, "Failed to acquire story handle for story='", story, "' code:", status);
        throw std::runtime_error("Failed to acquire story handle for story: " + story +
                                 ", error code: " + std::to_string(status));
    }

    struct StoryHandleGuard
    {
        chronolog::Client* client;
        const std::string& chronicle;
        const std::string& key;
        ~StoryHandleGuard() { client->ReleaseStory(chronicle, key); }
    } guard{client_.get(), chronicle_, story};

    std::vector<chronolog::Event> events;
    if(int ret = client_->ReplayStory(chronicle_, story, start_ts, end_ts, events); ret != chronolog::CL_SUCCESS)
    {
        CHRONOSQL_ERROR(logLevel_, "Failed to replay events for story='", story, "' code:", ret);
        throw std::runtime_error("Failed to replay events for story: " + story +
                                 ", error code: " + std::to_string(ret));
    }
    std::vector<EventPayload> out;
    out.reserve(events.size());
    for(const auto& e: events) { out.push_back({e.time(), e.log_record()}); }
    return out;
}

void ChronoSQLClientAdapter::flush()
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for(const auto& [story, handle]: handleCache_) { client_->ReleaseStory(chronicle_, story); }
    handleCache_.clear();
}

} // namespace chronosql
