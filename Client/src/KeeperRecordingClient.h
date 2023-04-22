
#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "../chrono_common/chronolog_types.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingClient
{

public:
    static KeeperRecordingClient * CreateKeeperRecordingClient( tl::engine & tl_engine,
		    std::string const & service_addr, uint16_t provider_id )
    	{
	   try{
	      return new KeeperRecordingClient( tl_engine,service_addr, provider_id);
	   } catch( tl::exception const&)
	   {
		std::cout<<"KeeperRecordingClient: failed construction"<<std::endl;
		return nullptr;
	   }
        }

    void send_event_msg( LogEvent const& eventMsg)
    {
	std::cout<< "KeeperRecordingClient::send_event_msg:"<<eventMsg.storyId<<std::endl;
	record_event.on(service_ph)(eventMsg);
    }

    ~KeeperRecordingClient()
    {
           record_event.deregister(); 
    }

    private:


    std::string service_addr;     // na address of Keeper Recording Service 
    uint16_t 	service_provider_id;          // KeeperRecordingService provider id
    tl::provider_handle  service_ph;  //provider_handle for remote registry service
    tl::remote_procedure record_event;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient( tl::engine & tl_engine, std::string const& service_addr, uint16_t provider_id)
	    : service_addr(service_addr), service_provider_id(provider_id)
	    , service_ph(tl_engine.lookup( service_addr),provider_id)
	{
	 std::cout<< "RecordingClient created for RecordingService at {"<<service_addr<<"} provider_id {"<<service_provider_id<<"}"<<std::endl;
   	 record_event =tl_engine.define("record_event").disable_response();
       
	}	
};

}

#endif
