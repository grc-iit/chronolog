#ifndef PlayerStoreAdmin_SERVICE_H
#define PlayerStoreAdmin_SERVICE_H

#include <string>
#include <cstdint>
#include <iostream>
#include <vector>
#include <margo.h>
#include <thallium.hpp>
#include <private/RpcReply.h>
#include <private/RpcBulkOwner.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <ServiceId.h>

#include "PlayerDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class PlayerStoreAdminService: public tl::provider<PlayerStoreAdminService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlayerStoreAdminService* CreatePlayerStoreAdminService(tl::engine& tl_engine,
                                                                  uint16_t service_provider_id,
                                                                  PlayerDataStore& dataStoreInstance)
    {
        auto* service = new PlayerStoreAdminService(tl_engine, service_provider_id, dataStoreInstance);
        service->legacy_ = true;
        service->registerProcedures();
        tl_engine.push_finalize_callback(service, [service] { delete service; });
        return service;
    }

    static PlayerStoreAdminService* CreatePlayerStoreAdminService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                                  uint16_t provider,
                                                                  PlayerDataStore& store)
    {
        if(!owner || !owner->engine || owner->provider)
            return nullptr;
        auto service =
                std::shared_ptr<PlayerStoreAdminService>(new PlayerStoreAdminService(*owner->engine, provider, store));
        service->owner_ = owner;
        service->owned_ = true;
        owner->provider = service;
        service->registerProcedures();
        return service.get();
    }
    ~PlayerStoreAdminService()
    {
        LOG_DEBUG("[PlayerStoreAdminService] Destructor called. Cleaning up...");
        //remove provider finalization callback from the engine's list
        if(legacy_)
            get_engine().pop_finalize_callback(this);
    }

    void collection_service_available(tl::request const& request) { rpc::respond(request, 1); }

    void shutdown_data_collection(tl::request const& request)
    {
        int status = 1;
        theDataStore.shutdownDataCollection();
        rpc::respond(request, status);
    }

    void StartStoryRecording(tl::request const& request,
                             std::string const& chronicle_name,
                             std::string const& story_name,
                             StoryId const& story_id,
                             uint64_t start_time)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
        {
            rpc::respond(request, CL_ERR_SHUTDOWN_INCOMPLETE);
            return;
        }

        LOG_INFO("[PlayerStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}", story_name, story_id);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        rpc::respond(request, return_code);
    }

    // Roster-carrying variant used by the visor: same story start, plus the
    // keeper recording services the playback service fans hot fetches to.
    void StartStoryRecordingWithKeepers(tl::request const& request,
                                        std::string const& chronicle_name,
                                        std::string const& story_name,
                                        StoryId const& story_id,
                                        uint64_t start_time,
                                        std::vector<ServiceId> const& keeper_services)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
        {
            rpc::respond(request, CL_ERR_SHUTDOWN_INCOMPLETE);
            return;
        }

        LOG_INFO("[PlayerStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}, {} keeper(s)",
                 story_name,
                 story_id,
                 keeper_services.size());
        theDataStore.setStoryKeepers(story_id, keeper_services);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        rpc::respond(request, return_code);
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        LOG_INFO("[PlayerStoreAdminService] Stopping Story Recording: StoryID={}", story_id);
        int return_code = theDataStore.stopStoryRecording(story_id);
        rpc::respond(request, return_code);
    }

private:
    PlayerStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id, PlayerDataStore& data_store_instance)
        : tl::provider<PlayerStoreAdminService>(tl_engine, service_provider_id)
        , theDataStore(data_store_instance)
    {}
    void registerProcedures()
    {
        define("collection_service_available", &PlayerStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &PlayerStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &PlayerStoreAdminService::StartStoryRecording);
        define("start_story_recording_with_keepers", &PlayerStoreAdminService::StartStoryRecordingWithKeepers);
        define("stop_story_recording", &PlayerStoreAdminService::StopStoryRecording);

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[PlayerStoreAdminService] Constructed at {}. ProviderID={}", ss.str(), get_provider_id());
    }

    PlayerStoreAdminService(PlayerStoreAdminService const&) = delete;

    PlayerStoreAdminService& operator=(PlayerStoreAdminService const&) = delete;

    bool legacy_ = false, owned_ = false;
    std::weak_ptr<rpc::RpcBulkOwner> owner_;
    PlayerDataStore& theDataStore;
};

} // namespace chronolog

#endif
