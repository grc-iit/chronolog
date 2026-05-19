#ifndef DataStoreAdmin_SERVICE_H
#define DataStoreAdmin_SERVICE_H

#include <iostream>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <chronolog_types.h>

#include "GrapherDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class DataStoreAdminService: public tl::provider <DataStoreAdminService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static DataStoreAdminService*
    CreateDataStoreAdminService(tl::engine &tl_engine, uint16_t service_provider_id, GrapherDataStore &dataStoreInstance)
    {
        return new DataStoreAdminService(tl_engine, service_provider_id, dataStoreInstance);
    }

    ~DataStoreAdminService()
    {
        LOG_DEBUG("[DataStoreAdminService] Destructor called. Cleaning up...");
        //remove provider finalization callback from the engine's list
        get_engine().pop_finalize_callback(this);
    }

    void collection_service_available(tl::request const &request)
    {
        request.respond(1);
    }

    void shutdown_data_collection(tl::request const &request)
    {
        int status = 1;
        theDataStore.shutdownDataCollection();
        request.respond(status);
    }

    void
    StartStoryRecording(tl::request const &request, std::string const &chronicle_name, std::string const &story_name
                        , StoryId const &story_id, uint64_t start_time)
    {
        LOG_INFO("[DataStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}", story_name, story_id);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        request.respond(return_code);
    }

    void StopStoryRecording(tl::request const &request, StoryId const &story_id)
    {
        StopStoryRecordingWithKeeperCount(request, story_id, 0);
    }

    void StopStoryRecordingWithKeeperCount(tl::request const &request,
                                           StoryId const &story_id,
                                           uint32_t expected_keeper_drains)
    {
        auto const total_start = std::chrono::steady_clock::now();
        auto elapsed_us = [](std::chrono::steady_clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                    .count();
        };
        LOG_INFO("[DataStoreAdminService] Stopping Story Recording: StoryID={} expectedKeeperDrains={}",
                 story_id,
                 expected_keeper_drains);
        uint64_t const drain_timeout_ms = stopStoryArchiveDrainTimeoutMs();
        auto const datastore_stop_start = std::chrono::steady_clock::now();
        int return_code = theDataStore.stopStoryRecording(story_id, drain_timeout_ms, expected_keeper_drains);
        int64_t const datastore_stop_us = elapsed_us(datastore_stop_start);
        LOG_INFO("[GrapherStopStoryProfile] story_id={} return_code={} drain_timeout_ms={} expected_keeper_drains={} "
                 "datastore_stop_us={} total_us={}",
                 story_id,
                 return_code,
                 drain_timeout_ms,
                 expected_keeper_drains,
                 datastore_stop_us,
                 elapsed_us(total_start));
        request.respond(return_code);
    }

private:
    static uint64_t stopStoryArchiveDrainTimeoutMs()
    {
        char const* enabled = std::getenv("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN");
        if(enabled == nullptr || *enabled == '\0' || std::string(enabled) == "0")
        {
            return 0;
        }

        char const* value = std::getenv("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS");
        if(value == nullptr || *value == '\0')
        {
            return 30000;
        }

        char* end = nullptr;
        unsigned long long parsed = std::strtoull(value, &end, 10);
        if(end == value || parsed > std::numeric_limits<uint64_t>::max())
        {
            return 30000;
        }
        return static_cast<uint64_t>(parsed);
    }

    DataStoreAdminService(tl::engine &tl_engine, uint16_t service_provider_id, GrapherDataStore &data_store_instance)
            : tl::provider <DataStoreAdminService>(tl_engine, service_provider_id), theDataStore(data_store_instance)
    {
        define("collection_service_available", &DataStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &DataStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
        define("stop_story_recording_with_keeper_count", &DataStoreAdminService::StopStoryRecordingWithKeeperCount);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[DataStoreAdminService] Constructed at {}. ProviderID={}", ss.str(), service_provider_id);
    }

    DataStoreAdminService(DataStoreAdminService const &) = delete;

    DataStoreAdminService &operator=(DataStoreAdminService const &) = delete;

    GrapherDataStore &theDataStore;
};

}// namespace chronolog

#endif
