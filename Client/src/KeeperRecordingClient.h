#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <iostream>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "chronolog_errcode.h"

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingClient
{

public:
    static KeeperRecordingClient*CreateKeeperRecordingClient(tl::engine &tl_engine, KeeperIdCard const &keeper_id_card
                                                             , std::string const &keeper_rpc_string)
    {
        try
        {
            return new KeeperRecordingClient(tl_engine, keeper_id_card, keeper_rpc_string);
        }
        catch(tl::exception const &)
        {
            Logger::getLogger()->error(
                    "[KeeperRecordingClient] Failed to create KeeperRecordingClient due to an exception.");
        }
        return nullptr;
    }

    int send_event_msg(LogEvent const &eventMsg)
    {
        try
        {
            std::stringstream ss;
            ss << eventMsg;
            Logger::getLogger()->info("[KeeperRecordingClient] Sending event message: {}", ss.str());
            int return_code = record_event.on(service_ph)(eventMsg);
            Logger::getLogger()->info("[KeeperRecordingClient] Sent event message: {} with return code: {}", ss.str()
                                      , return_code);
            return return_code;
        }
        catch(thallium::exception const &)
        {
            Logger::getLogger()->error("[KeeperRecordingClient] Failed to send event message due to an exception.");
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    KeeperIdCard const &getKeeperId() const
    { return keeperIdCard; }

    ~KeeperRecordingClient()
    {
        record_event.deregister();
        Logger::getLogger()->info("[KeeperRecordingClient] Destructor called. Deregistered record_event.");
    }

private:

    KeeperIdCard keeperIdCard;
    std::string service_addr;     // na address of Keeper Recording Service 
    tl::provider_handle service_ph;  //provider_handle for remote registry service
    tl::remote_procedure record_event;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine &tl_engine, KeeperIdCard const &keeper_id_card
                          , std::string const &keeper_service_addr): keeperIdCard(keeper_id_card), service_addr(
            keeper_service_addr), service_ph(tl_engine.lookup(service_addr), keeper_id_card.getProviderId())
    {
        Logger::getLogger()->info(
                "[KeeperRecordingClient] RecordingClient created for KeeperRecordingService at {} with ProviderID={}"
                , service_addr, keeperIdCard.getProviderId());
        record_event = tl_engine.define("record_event");
    }
};
}

#endif
