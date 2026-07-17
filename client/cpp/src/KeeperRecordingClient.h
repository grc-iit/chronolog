#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <iostream>
#include <string>

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/tuple.hpp>

#include <chronolog_types.h>
#include <ServiceId.h>
#include <client_errcode.h>
#include <chrono_monitor.h>

namespace tl = thallium;


namespace chronolog
{


class KeeperRecordingClient
{

public:
    static KeeperRecordingClient* CreateKeeperRecordingClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
    {
        try
        {
            return new KeeperRecordingClient(tl_engine, keeper_service_id);
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to create KeeperRecordingClient exception {}", ex.what());
        }
        return nullptr;
    }

    int send_event_msg(LogEvent const& eventMsg)
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
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to send event message to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return (chronolog::CL_ERR_UNKNOWN);
    }

    ServiceId const& getRecordingServiceId() const { return recordingServiceId; }

    // Tail-read phase 1: ask this keeper for its most recent (up to n) event
    // sequences for the story.
    std::vector<EventSequence> getTailSequences(StoryId const& story_id, uint64_t n)
    {
        try
        {
            return tail_get_sequences.on(service_ph)(story_id, n);
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] getTailSequences to {} failed: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return std::vector<EventSequence>{};
    }

    // Tail-read phase 2: ask this keeper for the payloads of the given sequences.
    std::vector<LogEvent> getTailEvents(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        try
        {
            return tail_get_events.on(service_ph)(story_id, seqs);
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] getTailEvents to {} failed: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return std::vector<LogEvent>{};
    }

    ~KeeperRecordingClient()
    {
        record_event.deregister();
        tail_get_sequences.deregister();
        tail_get_events.deregister();
        LOG_DEBUG("[KeeperRecordingClient] Destructor called {}", to_string(recordingServiceId));
    }

private:
    ServiceId recordingServiceId;
    tl::provider_handle service_ph; //provider_handle for remote registry service
    tl::remote_procedure record_event;
    tl::remote_procedure tail_get_sequences;
    tl::remote_procedure tail_get_events;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
        : recordingServiceId(keeper_service_id)
    {
        LOG_DEBUG("[KeeperRecordingClient] KeeperRecordingiClient Constructor for {}", to_string(keeper_service_id));
        std::string service_addr_string;
        recordingServiceId.get_service_as_string(service_addr_string);

        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), recordingServiceId.getProviderId());

        record_event = tl_engine.define("record_event");
        tail_get_sequences = tl_engine.define("tail_get_sequences");
        tail_get_events = tl_engine.define("tail_get_events");
    }


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const&) = delete;
    KeeperRecordingClient& operator=(KeeperRecordingClient const&) = delete;
};
} // namespace chronolog

#endif
