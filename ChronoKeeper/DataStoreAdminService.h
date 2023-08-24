#ifndef DataStoreAdmin_SERVICE_H
#define DataStoreAdmin_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chrono_common/chronolog_types.h"
#include "KeeperDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class DataStoreAdminService : public tl::provider<DataStoreAdminService> 
{
public:
    
// Service should be created on the heap not the stack thus the constructor is private...

    static DataStoreAdminService * CreateDataStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperDataStore & dataStoreInstance)
    {
          return  new DataStoreAdminService( tl_engine, service_provider_id, dataStoreInstance );
    }  

    ~DataStoreAdminService() 
    {
        std::cout << "DataStoreAdminService::~DataStoreAdminService"<< std::endl;
	//remove provider finalization callback from the engine's list	
        get_engine().pop_finalize_callback(this);
    }

    void collection_service_available(tl::request const& request)
    {   
	    request.respond(1);
    }

    void shutdown_data_collection(tl::request const& request)
    {   
	    int status = 1;
	    theDataStore.shutdownDataCollection();
	    request.respond(status);
    }

    void StartStoryRecording(tl::request const& request, 
                      std::string const& chronicle_name, std::string const& story_name,  StoryId const& story_id, uint64_t start_time)
    {
        std::cout << "DataCollectionService: StartStoryRecoding {"<< story_name<<":"<<story_id<<"}"<< std::endl;
        theDataStore.startStoryRecording(chronicle_name, story_name, story_id, start_time);
        request.respond( 0);
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        std::cout << "DataStoreAdminService: StopStoryRecoding {"<< story_id<<"}"<< std::endl;
	theDataStore.stopStoryRecording(story_id);
        request.respond( 0);
    }

private:
    DataStoreAdminService(tl::engine& tl_engine, uint16_t service_provider_id, KeeperDataStore & data_store_instance)
    	: tl::provider<DataStoreAdminService>(tl_engine, service_provider_id)
	, theDataStore(data_store_instance)  
    {
        define("collection_service_available", &DataStoreAdminService::collection_service_available);
        define("shutdown_data_collection", &DataStoreAdminService::shutdown_data_collection);
        define("start_story_recording", &DataStoreAdminService::StartStoryRecording);
        define("stop_story_recording", &DataStoreAdminService::StopStoryRecording);
	//set up callback for the case when the engine is being finalized while this provider is still alive
	get_engine().push_finalize_callback(this, [p=this](){delete p;} );
	std::cout<<"DataStoreAdminService::constructed at "<< get_engine().self()<<" provider_id {"<<service_provider_id<<"}"<<std::endl;
    }

    DataStoreAdminService( DataStoreAdminService const&) = delete;
    DataStoreAdminService & operator= (DataStoreAdminService const&) = delete;

    KeeperDataStore& theDataStore;
};

}// namespace chronolog

#endif
