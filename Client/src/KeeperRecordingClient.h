
#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingClient
{

public:
    static KeeperRecordingClient * CreateKeeperRecordingClient( tl::engine & tl_engine
            , KeeperIdCard const& keeper_id_card
		    , std::string const & keeper_rpc_string)
    {
	    try
        {
            return new KeeperRecordingClient( tl_engine, keeper_id_card, keeper_rpc_string);
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

    KeeperIdCard const& getKeeperId() const
    {   return keeperIdCard;  }

    ~KeeperRecordingClient()
    {
        record_event.deregister(); 
    }

private:

    KeeperIdCard    keeperIdCard;
    std::string service_addr;     // na address of Keeper Recording Service 
    tl::provider_handle  service_ph;  //provider_handle for remote registry service
    tl::remote_procedure record_event;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient( tl::engine & tl_engine, KeeperIdCard const& keeper_id_card, std::string const& keeper_service_addr)
	    : keeperIdCard(keeper_id_card)
        , service_addr( keeper_service_addr)
	    , service_ph(tl_engine.lookup( service_addr), keeper_id_card.getProviderId())
	{
        std::cout<< "RecordingClient created for KeeperRecordingService at {"<<service_addr<<"} provider_id {"<<keeperIdCard.getProviderId()<<"}"<<std::endl;
        record_event =tl_engine.define("record_event").disable_response();
	}	
};

}

#endif
