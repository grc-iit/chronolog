#ifndef DataStoreAdmin_SERVICE_H
#define DataStoreAdmin_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <private/RpcReply.h>
#include <thallium/serialization/stl/string.hpp>

#include <chronolog_types.h>
#include <private/RpcBulkOwner.h>

#include "GrapherDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class DataStoreAdminService: public tl::provider<DataStoreAdminService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static DataStoreAdminService* CreateDataStoreAdminService(tl::engine& tl_engine,
                                                              uint16_t service_provider_id,
                                                              GrapherDataStore& dataStoreInstance)
    {
        auto* service = new DataStoreAdminService(tl_engine, service_provider_id, dataStoreInstance);
        service->registerProcedures();
        service->legacy_ = true;
        tl_engine.push_finalize_callback(service, [service] { delete service; });
        return service;
    }

    static DataStoreAdminService* CreateDataStoreAdminService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                              uint16_t provider,
                                                              GrapherDataStore& store)
    {
        if(!owner || owner->closing)
            return nullptr;
        if(owner->provider)
            throw std::logic_error("Grapher admin provider already exists");
        auto service =
                std::shared_ptr<DataStoreAdminService>(new DataStoreAdminService(*owner->engine, provider, store));
        service->owner_ = owner;
        service->owned_ = true;
        owner->provider = service; // Retain partial registration and its borrowed store.
        service->registerProcedures();
        return service.get();
    }

    ~DataStoreAdminService()
    {
        LOG_DEBUG("[DataStoreAdminService] Destructor called. Cleaning up...");
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
        LOG_INFO("[DataStoreAdminService] Starting Story Recording: StoryName={}, StoryID={}", story_name, story_id);
        int return_code = theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        rpc::respond(request, return_code);
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        LOG_INFO("[DataStoreAdminService] Stopping Story Recording: StoryID={}", story_id);
        int return_code = theDataStore.stopStoryRecording(story_id);
        rpc::respond(request, return_code);
    }

    void DestroyStory(tl::request const& request,
                      std::string const& chronicle_name,
                      std::string const& story_name,
                      StoryId const& story_id)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
        {
            rpc::respond(request, CL_ERR_SHUTDOWN_INCOMPLETE);
            return;
        }
        LOG_INFO("[DataStoreAdminService] Destroying Story: Chronicle={}, Story={}, StoryID={}",
                 chronicle_name,
                 story_name,
                 story_id);
        int return_code = theDataStore.destroyStory(story_id, chronicle_name, story_name);
        rpc::respond(request, return_code);
    }

    void DestroyChronicle(tl::request const& request, std::string const& chronicle_name)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
        {
            rpc::respond(request, CL_ERR_SHUTDOWN_INCOMPLETE);
            return;
        }
        LOG_INFO("[DataStoreAdminService] Destroying Chronicle: {}", chronicle_name);
        int return_code = theDataStore.destroyChronicle(chronicle_name);
        rpc::respond(request, return_code);
    }

private:
    DataStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id, GrapherDataStore& data_store_instance)
        : tl::provider<DataStoreAdminService>(tl_engine, service_provider_id)
        , theDataStore(data_store_instance)
    {}
    void registerProcedures()
    {
        define("collection_service_available", &DataStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &DataStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
        define("destroy_story", &DataStoreAdminService::DestroyStory);
        define("destroy_chronicle", &DataStoreAdminService::DestroyChronicle);

        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[DataStoreAdminService] Constructed at {}. ProviderID={}", ss.str(), get_provider_id());
    }

    DataStoreAdminService(DataStoreAdminService const&) = delete;

    DataStoreAdminService& operator=(DataStoreAdminService const&) = delete;

    bool legacy_ = false, owned_ = false;
    std::weak_ptr<rpc::RpcBulkOwner> owner_;
    GrapherDataStore& theDataStore;
};

} // namespace chronolog

#endif
