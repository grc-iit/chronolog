#ifndef DATA_COLLECTION_SERVICE_H
#define DATA_COLLECTION_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include "KeeperIdCard.h"

#include "chronolog_types.h"
#include "KeeperDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class DataCollectionService : public tl::provider<DataCollectionService> 
{
public:
    
// Service should be created on the heap not the stack thus the constructor is private...

    static DataCollectionService * CreateDataCollectionService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperDataStore & dataStoreInstance)
    {
          return  new DataCollectionService( tl_engine, service_provider_id, dataStoreInstance );
    }  

    ~DataCollectionService() 
    {
	//remove provider finalization callback from the engine's list	
        get_engine().pop_finalize_callback(this);
    }

    void collection_service_available(tl::request const& request)//, uint16_t provider_id)
    {   
	    request.respond(1);//provider_id); //maybe respond with KeeperId ?
    }

    void shutdown_data_collection(tl::request const& request)//, uint16_t provider_id)
    {   
	    int status =1;
	    theDataStore.shutdownDataCollection();
	    request.respond(1); //maybe respond with KeeperId ?
    }

    void StartStoryRecording(tl::request const& request, 
                      std::string const& chronicle_name, std::string const& story_name,  StoryId const& story_id)
    {
        std::cout << "DataCollectionService: StartStoryRecoding {"<< story_name<<":"<<story_id<<"}"<< std::endl;
        theDataStore.startStoryRecording(chronicle_name, story_name, story_id);
        request.respond( story_id );
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        std::cout << "DataCollectionService: StartStoryRecoding {"<< story_id<<"}"<< std::endl;
	theDataStore.stopStoryRecording(story_id);
        request.respond( story_id );
    }

private:
    DataCollectionService(tl::engine& tl_engine, uint16_t service_provider_id, KeeperDataStore & data_store_instance)
    	: tl::provider<DataCollectionService>(tl_engine, service_provider_id)
	, theDataStore(data_store_instance)  
    {
        define("collection_service_available", &DataCollectionService::collection_service_available);
        define("shutdown_data_collection", &DataCollectionService::shutdown_data_collection);
        define("start_story_recording", &DataCollectionService::StartStoryRecording);
        define("stop_story_recording", &DataCollectionService::StopStoryRecording);
	//set up callback for the case when the engine is being finalized while this provider is still alive
	get_engine().push_finalize_callback(this, [p=this](){delete p;} );
	std::cout<<"DataCollectionService::constructed at "<< get_engine().self()<<" provider_id {"<<service_provider_id<<"}"<<std::endl;
    }

    DataCollectionService( DataCollectionService const&) = delete;
    DataCollectionService & operator= (DataCollectionService const&) = delete;

    KeeperDataStore& theDataStore;
};

}// namespace chronolog

#endif
