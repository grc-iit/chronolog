#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

// How one record_event_durable attempt ended. Only Answered carries a keeper
// code; the caller retries TimedOut and TransportError with the same event.
enum class DurableSendOutcome
{
    Answered,      // the keeper answered; keeper_code holds its status
    TimedOut,      // no answer within the attempt's deadline
    NotRegistered, // the keeper does not define record_event_durable (a 3.3.0 keeper), or is
                   // dead and never answered this client
    TransportError // any other RPC failure
};

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

    // One attempt of a durable write: record_event_durable with a deadline of
    // timeout_ms. A keeper that lacks the RPC answers the forward with
    // HG_NO_MATCH, which thallium raises as a margo_exception; that maps to
    // NotRegistered, so the caller does not retry a call that cannot work.
    // HG_NO_MATCH is HG_NOENTRY, which a forward to a dead keeper also gets
    // over ofi+sockets. Once this keeper has answered record_event_durable, it
    // defines the RPC, so HG_NO_MATCH from it is a TransportError. Before
    // that, keeperIsAlive tells the two apart: a keeper that answers
    // tail_get_sequences, which v3.3.0 keepers also define, is up and lacks
    // the RPC; one that does not is down, and the caller retries and may reach
    // it restarted at the same address.
    DurableSendOutcome send_durable_event_msg(ChronicleName const& chronicle,
                                              StoryName const& story,
                                              LogEvent const& eventMsg,
                                              uint32_t timeout_ms,
                                              int& keeper_code)
    {
        try
        {
            keeper_code = record_event_durable.on(service_ph)
                                  .timed(std::chrono::milliseconds(timeout_ms), chronicle, story, eventMsg);
            answeredDurable = true;
            return DurableSendOutcome::Answered;
        }
        catch(tl::timeout const&)
        {
            LOG_WARNING("[KeeperRecordingClient] Durable event to {} timed out after {} ms",
                        to_string(recordingServiceId),
                        timeout_ms);
            return DurableSendOutcome::TimedOut;
        }
        catch(tl::margo_exception const& ex)
        {
            if(ex.error() == HG_NO_MATCH && !answeredDurable && keeperIsAlive(timeout_ms))
            {
                LOG_ERROR("[KeeperRecordingClient] Keeper {} does not define record_event_durable",
                          to_string(recordingServiceId));
                return DurableSendOutcome::NotRegistered;
            }
            LOG_ERROR("[KeeperRecordingClient] Failed to send durable event to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to send durable event to {} exception: {}",
                      to_string(recordingServiceId),
                      ex.what());
        }
        return DurableSendOutcome::TransportError;
    }

    // Whether the keeper answers an RPC that every keeper since 3.1 defines: a
    // tail read of no events of no story, which changes nothing there. Waits
    // at most timeout_ms, and no more than kTailReadRpcTimeoutMs.
    bool keeperIsAlive(uint32_t timeout_ms)
    {
        auto const deadline =
                std::chrono::milliseconds(std::min<uint32_t>(timeout_ms, static_cast<uint32_t>(kTailReadRpcTimeoutMs)));
        try
        {
            std::vector<EventSequence> const none =
                    tail_get_sequences.on(service_ph).timed(deadline, StoryId(0), uint64_t(0));
            (void)none;
            return true;
        }
        catch(tl::exception const& ex)
        {
            LOG_WARNING("[KeeperRecordingClient] Keeper {} does not answer: {}",
                        to_string(recordingServiceId),
                        ex.what());
        }
        return false;
    }

    ServiceId const& getRecordingServiceId() const { return recordingServiceId; }

    // Per-RPC deadline (ms) for tail reads so a slow or hung keeper cannot block
    // playback() indefinitely. Tail reads are served from the keeper's in-memory
    // tail and normally return in well under a second; this is a generous ceiling
    // (a hung keeper never responds, so any finite bound achieves the goal).
    static constexpr int kTailReadRpcTimeoutMs = 5000;

    // Tail-read phase 1 (async): issue tail_get_sequences without blocking and
    // return a handle to wait on. Lets a caller fan out to all of a story's
    // keepers concurrently so one slow/hung keeper cannot serialize the rest.
    // wait() yields the sequences, or throws thallium::timeout after
    // kTailReadRpcTimeoutMs (see gather_story_tail).
    tl::async_response getTailSequencesAsync(StoryId const& story_id, uint64_t n)
    {
        return tail_get_sequences.on(service_ph)
                .timed_async(std::chrono::milliseconds(kTailReadRpcTimeoutMs), story_id, n);
    }

    // Tail-read phase 2 (async): issue tail_get_events without blocking and return
    // a handle to wait on, so payloads can be fetched from all of a story's
    // keepers concurrently. wait() yields the payloads, or throws
    // thallium::timeout after kTailReadRpcTimeoutMs. `seqs` must stay alive until
    // the returned handle is waited on.
    tl::async_response getTailEventsAsync(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        return tail_get_events.on(service_ph)
                .timed_async(std::chrono::milliseconds(kTailReadRpcTimeoutMs), story_id, seqs);
    }

    ~KeeperRecordingClient()
    {
        record_event.deregister();
        record_event_durable.deregister();
        tail_get_sequences.deregister();
        tail_get_events.deregister();
        LOG_DEBUG("[KeeperRecordingClient] Destructor called {}", to_string(recordingServiceId));
    }

private:
    ServiceId recordingServiceId;
    tl::provider_handle service_ph; //provider_handle for remote registry service
    tl::remote_procedure record_event;
    tl::remote_procedure record_event_durable;
    tl::remote_procedure tail_get_sequences;
    tl::remote_procedure tail_get_events;
    // set by the first answer to record_event_durable; never cleared
    std::atomic<bool> answeredDurable{false};

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
        : recordingServiceId(keeper_service_id)
    {
        LOG_DEBUG("[KeeperRecordingClient] KeeperRecordingiClient Constructor for {}", to_string(keeper_service_id));
        std::string service_addr_string;
        recordingServiceId.get_service_as_string(service_addr_string);

        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), recordingServiceId.getProviderId());

        record_event = tl_engine.define("record_event");
        record_event_durable = tl_engine.define("record_event_durable");
        tail_get_sequences = tl_engine.define("tail_get_sequences");
        tail_get_events = tl_engine.define("tail_get_events");
    }


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const&) = delete;
    KeeperRecordingClient& operator=(KeeperRecordingClient const&) = delete;
};
} // namespace chronolog

#endif
