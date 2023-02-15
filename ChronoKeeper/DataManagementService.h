#ifndef DATA_MANAGEMENT_SERVICE_H
#define DATA_MANAGEMENT_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include "KeeperIdCard.h"

namespace tl = thallium;


namespace chronolog
{

//#INNA :  for now assume all identifiers are uint64_t , revisit later 
typedef uint64_t StoryId;
typedef uint64_t StorytellerId;


class DataManagementService : public tl::provider<DataManagementService> 
{
public:
    
// Service should be created on the heap not the stack thus the constructor is private...

    static DataManagementService * CreateDataManagementService(tl::engine& tl_engine, uint16_t service_provider_id
		    , KeeperDataStore & dataStoreInstance)
    {
          return  new DataManagementService( tl_engine, service_provider_id, dataStoreInstance );
    }  

    ~DataManagementService() 
    {
	
        get_engine().pop_finalize_callback(this);
    }


    void StartStoryRecording(tl::request const& request, 
                      std::string const& chronicle_name, std::string const& story_name,  StoryId const& story_id)
    {
        std::cout << "DataManagement: StartStoryRecoding {"<< story_name<<":"<<story_id<<"}"<< std::endl;
        theDataStore.startStoryRecording(chronicle_name, story_name, story_id);
        request.respond( story_id );
    }

    void StopStoryRecording(tl::request const& request, StoryId const& story_id)
    {
        std::cout << "DataManagement: StartStoryRecoding {"<< story_name<<":"<<story_id<<"}"<< std::endl;
	theDataStore.stopStoryRecording(story_id);
        request.respond( story_id );
    }

    void shutdown_services(tl::request & request)
    {   request.respond(1); //maybe respond with KeeperId ?

private:
    DataManagementService(tl::engine& tl_engine, uint16_t service_provider_id, KeeperDataStore & dataStoreInstance)
    	: tl::provider<DataManagementService>(tl_engine, service_provider_id)
	, theDataStore(datastoreInstance)  
    {
        define("StartStoryRecording", &DataManagementService::StartStoryRecording);
        define("StopStoryRecording", &DataManagementService::StopStoryRecording);
	//set up callback for the case when the engine is being finalized while this provider is still alive
	get_engine().push_finalize_callback(this, [p=this](){delete p;} );
    }

    DataManagementService( DataManagementService const&) = delete;
    DataManagementService & operator= (DataManagementService const&) = delete;

    KeeperDataStore& theDataStore;
};

}// namespace chronolog

#endif
