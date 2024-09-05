#ifndef DataStoreAdmin_SERVICE_H
#define DataStoreAdmin_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"
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
        LOG_INFO("[DataStoreAdminService] Stopping Story Recording: StoryID={}", story_id);
        int return_code = theDataStore.stopStoryRecording(story_id);
        request.respond(return_code);
    }

private:
    DataStoreAdminService(tl::engine &tl_engine, uint16_t service_provider_id, GrapherDataStore &data_store_instance)
            : tl::provider <DataStoreAdminService>(tl_engine, service_provider_id), theDataStore(data_store_instance)
    {
        define("collection_service_available", &DataStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &DataStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
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
