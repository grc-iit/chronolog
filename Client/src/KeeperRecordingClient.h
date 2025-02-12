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
    static KeeperRecordingClient*
    CreateKeeperRecordingClient(tl::engine &tl_engine, KeeperIdCard const &keeper_id_card)
    {
        try
        {
            return new KeeperRecordingClient(tl_engine, keeper_id_card);
        }
        catch(tl::exception const & ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to create KeeperRecordingClient exception {}",ex.what());
        }
        return nullptr;
    }

    int send_event_msg(LogEvent const &eventMsg)
    {
        try
        {
            //std::stringstream ss;
            //ss << eventMsg;
            //LOG_TRACE("[KeeperRecordingClient] Sending event message: {}", ss.str());
            int return_code = record_event.on(service_ph)(eventMsg);
            //LOG_TRACE("[KeeperRecordingClient] Sent event message: {} with return code: {}", ss.str(), return_code);
            return return_code;
        }
        catch(thallium::exception const & ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to send event message to {} exception: {}", to_string(keeperIdCard), ex.what());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    KeeperIdCard const & getKeeperId() const
    { return keeperIdCard; }

    ~KeeperRecordingClient()
    {
        record_event.deregister();
        LOG_DEBUG("[KeeperRecordingClient] Destructor called {}", to_string(keeperIdCard));
    }

private:

    KeeperIdCard keeperIdCard;
    tl::provider_handle service_ph;  //provider_handle for remote registry service
    tl::remote_procedure record_event;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine &tl_engine, KeeperIdCard const &keeper_id_card)
        : keeperIdCard(keeper_id_card)
    {
        LOG_DEBUG("[KeeperRecordingClient] KeeperRecordingiClient Constructor for {}",to_string(keeper_id_card));
        std::string service_addr_string;
        keeperIdCard.getRecordingServiceId().get_service_as_string(service_addr_string);

        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), keeper_id_card.getRecordingServiceId().getProviderId());

        record_event = tl_engine.define("record_event");
    }


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const &) = delete;
    KeeperRecordingClient &operator=(KeeperRecordingClient const &) = delete;

};
}

#endif
