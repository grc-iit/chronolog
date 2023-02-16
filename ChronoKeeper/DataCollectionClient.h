
#ifndef DATA_COLLECTION_CLIENT_H
#define DATA_COLLECTION_CLIENT_H

#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "KeeperIdCard.h"

namespace tl = thallium;

namespace chronolog
{

typedef std::string ChronicleName;
typedef std::string StoryName;
typedef uint64_t StoryId;

class DataCollectionClient
{

public:
    static DataCollectionClient  * CreateDataCollectionClient( tl::engine & tl_engine,
		    std::string const & collection_service_addr, uint16_t collection_provider_id )
    	{
      		return new DataCollectionClient( tl_engine, collection_service_addr, collection_provider_id);
        }


    bool remote_service_available()
    {
	bool available = service_available.on(service_handle)();
	std::cout <<"CollectionClient: service_available:"<<(available?"true":"false")<<std::endl;    
	return available;
    }

    int remote_service_shutdown()
    {
	bool shutting_down = shutdown_service.on(service_handle)();
	std::cout <<"CollectionClient: service_shutdown:"<<(shutting_down?"true":"false")<<std::endl;    
	return shutting_down;
    }

    int send_start_story_recording( ChronicleName const& chronicle_name, StoryName const& story_name,StoryId const& story_id )
    {
	 std::cout<< "CollectionClient: start_story_recording:"<<story_id<<std::endl;
	 return start_story_recording.on(service_handle)(chronicle_name,story_name,story_id);
    }
    int send_stop_story_recording( StoryId const& story_id)
    {
	 std::cout<< "CollectionClient: stop_story_recording"<<story_id<<std::endl;
	 return stop_story_recording.on(service_handle)(story_id);
    }

    ~DataCollectionClient()
    {
	   service_available.deregister(); 
           shutdown_service.deregister(); 
	   start_story_recording.deregister();
	   stop_story_recording.deregister();
    }

    private:


    std::string service_addr;     // na address of Keeper Collection Service 
    uint16_t 	service_provider_id;          // Keeper CollectionService provider id
    tl::provider_handle  service_handle;  //provider_handle for remote collector service
    tl::remote_procedure service_available;
    tl::remote_procedure shutdown_service;
    tl::remote_procedure start_story_recording;
    tl::remote_procedure stop_story_recording;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    DataCollectionClient( tl::engine & tl_engine, std::string const& collection_service_addr, uint16_t collection_provider_id)
	    : service_addr(collection_service_addr), service_provider_id(collection_provider_id)
	    , service_handle(tl_engine.lookup( collection_service_addr),collection_provider_id)
	{
   	 service_available = tl_engine.define("service_available");
   	 shutdown_service = tl_engine.define("shutdown_service");
   	 start_story_recording =tl_engine.define("start_story_recording"); 
   	 stop_story_recording =tl_engine.define("stop_story_recording");
       
	}	

};

}

#endif
