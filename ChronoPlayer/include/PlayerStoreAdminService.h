#ifndef PlayerStoreAdmin_SERVICE_H
#define PlayerStoreAdmin_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"
#include "PlayerDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class PlayerStoreAdminService: public tl::provider <PlayerStoreAdminService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlayerStoreAdminService*
    CreatePlayerStoreAdminService(tl::engine &tl_engine, uint16_t service_provider_id, PlayerDataStore &dataStoreInstance)
    {
        return new PlayerStoreAdminService(tl_engine, service_provider_id, dataStoreInstance);
    }

    ~PlayerStoreAdminService()
    {
        LOG_DEBUG("[PlayerStoreAdminService] Destructor called. Cleaning up...");
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
/*
    void
    StartStoryRecording(tl::request const &request, std::string const &chronicle_name, std::string const &story_name
                        , StoryId const &story_id, uint64_t start_time)
    {
        LOG_INFO("[PlayerStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}", story_name, story_id);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        request.respond(return_code);
    }

    void StopStoryRecording(tl::request const &request, StoryId const &story_id)
    {
        LOG_INFO("[PlayerStoreAdminService] Stopping Story Recording: StoryID={}", story_id);
        int return_code = theDataStore.stopStoryRecording(story_id);
        request.respond(return_code);
    }
*/
private:
    PlayerStoreAdminService(tl::engine &tl_engine, uint16_t service_provider_id, PlayerDataStore &data_store_instance)
            : tl::provider <PlayerStoreAdminService>(tl_engine, service_provider_id), theDataStore(data_store_instance)
    {
        define("collection_service_available", &PlayerStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &PlayerStoreAdminService::shutdown_data_collection);
        //define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        //define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[PlayerStoreAdminService] Constructed at {}. ProviderID={}", ss.str(), service_provider_id);
    }

    PlayerStoreAdminService(PlayerStoreAdminService const &) = delete;

    PlayerStoreAdminService &operator=(PlayerStoreAdminService const &) = delete;

    PlayerDataStore &theDataStore;
};

}// namespace chronolog

#endif
