#ifndef DataStoreAdmin_CLIENT_H
#define DataStoreAdmin_CLIENT_H

#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"

namespace tl = thallium;

namespace chronolog
{

//typedef std::string ChronicleName;
//typedef std::string StoryName;
//typedef uint64_t StoryId;

class DataStoreAdminClient
{

public:
    static DataStoreAdminClient*
    CreateDataStoreAdminClient(tl::engine &tl_engine, std::string const &collection_service_addr
                               , uint16_t collection_provider_id)
    {
        DataStoreAdminClient*adminClient = nullptr;
        try
        {
            adminClient = new DataStoreAdminClient(tl_engine, collection_service_addr, collection_provider_id);
        }
        catch(tl::exception const &ex)
        {
            std::cout << "ERROR: failed to create DataStoreAdminClient" << std::endl;
        }
        return adminClient;
    }


    int collection_is_available()
    {
        int available = 0;
        try
        {
            available = collection_service_available.on(service_handle)();
            std::cout << "DataStoreAdminClient: service_available:" << (available) << std::endl;
        }
        catch(tl::exception const &ex)
        {
            std::cout << "ERROR:DataStoreAdminClient: service unavailable:" << (available) << std::endl;
        }
        return available;
    }

    int shutdown_collection()
    {

        int status = CL_ERR_UNKNOWN;
        try
        {
            std::cout << "DataStoreAdminClient: shutdown_collection:" << std::endl;
            status = shutdown_data_collection.on(service_handle)();
        }
        catch(tl::exception const &ex)
        {}
        return status;
    }

    int
    send_start_story_recording(ChronicleName const &chronicle_name, StoryName const &story_name, StoryId const &story_id
                               , uint64_t start_time)
    {
        int status = CL_ERR_UNKNOWN;
        try
        {
            std::cout << "CollectionClient: start_story_recording:" << story_id << std::endl;
            status = start_story_recording.on(service_handle)(chronicle_name, story_name, story_id, start_time);
        }
        catch(tl::exception const &ex)
        {}
        return status;
    }

    int send_stop_story_recording(StoryId const &story_id)
    {
        int status = CL_ERR_UNKNOWN;
        try
        {
            std::cout << "DatastoreAdminClient: stop_story_recording" << story_id << std::endl;
            status = stop_story_recording.on(service_handle)(story_id);
        }
        catch(tl::exception const &ex)
        {}
        return status;
    }

    ~DataStoreAdminClient()
    {
        collection_service_available.deregister();
        shutdown_data_collection.deregister();
        start_story_recording.deregister();
        stop_story_recording.deregister();
    }

private:


    std::string service_addr;     // na address of Keeper Collection Service 
    uint16_t service_provider_id;          // Keeper CollectionService provider id
    tl::provider_handle service_handle;  //provider_handle for remote collector service
    tl::remote_procedure collection_service_available;
    tl::remote_procedure shutdown_data_collection;
    tl::remote_procedure start_story_recording;
    tl::remote_procedure stop_story_recording;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    DataStoreAdminClient(tl::engine &tl_engine, std::string const &collection_service_addr
                         , uint16_t collection_provider_id): service_addr(collection_service_addr), service_provider_id(
            collection_provider_id), service_handle(tl_engine.lookup(collection_service_addr), collection_provider_id)
    {
        collection_service_available = tl_engine.define("collection_service_available");
        shutdown_data_collection = tl_engine.define("shutdown_data_collection");
        start_story_recording = tl_engine.define("start_story_recording");
        stop_story_recording = tl_engine.define("stop_story_recording");

    }

};

}

#endif
