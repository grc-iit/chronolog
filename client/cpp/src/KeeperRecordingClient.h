#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <chrono>
#include "ClientRpcContext.h"
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
    static KeeperRecordingClient* CreateKeeperRecordingClient(tl::engine& tl_engine,
                                                              ServiceId const& keeper_service_id,
                                                              std::shared_ptr<ClientRpcProcedures> procedures)
    {
        if(!procedures)
            return nullptr;
        try
        {
            return new KeeperRecordingClient(tl_engine, keeper_service_id, std::move(procedures));
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperRecordingClient] Failed to create KeeperRecordingClient exception {}", ex.what());
        }
        return nullptr;
    }

    // Installed before calls by private tests; owned continuations retain the
    // observers. They do not replace the native call or its lifetime owner.
    struct AppendObservers
    {
        std::function<void()> beforeLookup, afterLookup, beforeDispatch, afterNative;
    };
    class AppendRequest
    {
    public:
        AppendRequest(tl::engine& engine,
                      ServiceId route,
                      std::shared_ptr<ClientRpcProcedures> procedures,
                      std::shared_ptr<AppendObservers const> observers)
            : engine_(engine)
            , route_(std::move(route))
            , procedures_(std::move(procedures))
            , observers_(std::move(observers))
        {}
        rpc::RpcResult<int> send_event_msg(rpc::OperationContext& operation, LogEvent const& event, uint64_t attemptMs)
        {
            try
            {
                if(observers_ && observers_->beforeLookup)
                    observers_->beforeLookup();
                if(operation.deadline().expired())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                std::string address;
                route_.get_service_as_string(address);
                endpoint_.emplace(engine_.lookup(address));
                if(observers_ && observers_->afterLookup)
                    observers_->afterLookup();
                peer_.emplace(*endpoint_, route_.getProviderId());
                call_.emplace(procedures_->calls[ClientRpcProcedures::RecordEvent].on(*peer_));
                if(observers_ && observers_->beforeDispatch)
                    observers_->beforeDispatch();
                auto timeout = operation.deadline().cappedAttempt(std::chrono::milliseconds(attemptMs));
                if(!timeout || !operation.tryBeginDispatch())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                rpc::RpcResult<int> result{CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
                try
                {
                    result.reply = call_->timed(*timeout, event);
                    result.status = CL_SUCCESS;
                }
                catch(...)
                {}
                // A timeout still has no settlement proof. Runtime retains this
                // request, payload, procedure and actual owner in that case.
                if(observers_ && observers_->afterNative)
                    observers_->afterNative();
                return result;
            }
            catch(...)
            {
                return {operation.deadline().expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_ERR_UNKNOWN,
                        false,
                        std::nullopt};
            }
        }

    private:
        tl::engine& engine_;
        ServiceId route_;
        std::shared_ptr<ClientRpcProcedures> procedures_;
        std::shared_ptr<AppendObservers const> observers_;
        std::optional<tl::endpoint> endpoint_;
        std::optional<tl::provider_handle> peer_;
        using Call = decltype(std::declval<tl::remote_procedure>().on(std::declval<tl::provider_handle>()));
        std::optional<Call> call_;
    };
    std::unique_ptr<AppendRequest> prepareAppend()
    {
        return std::make_unique<AppendRequest>(engine_, recordingServiceId, procedures_, appendObservers_);
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

    ~KeeperRecordingClient() = default;

private:
    friend struct KeeperRecordingClientTestAccess;
    tl::engine& engine_;
    std::shared_ptr<AppendObservers const> appendObservers_;
    std::shared_ptr<ClientRpcProcedures> procedures_;
    ServiceId recordingServiceId;
    tl::provider_handle service_ph; //provider_handle for remote registry service
    tl::remote_procedure tail_get_sequences;
    tl::remote_procedure tail_get_events;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine& tl_engine,
                          ServiceId const& keeper_service_id,
                          std::shared_ptr<ClientRpcProcedures> procedures)
        : engine_(tl_engine)
        , procedures_(std::move(procedures))
        , recordingServiceId(keeper_service_id)
    {
        LOG_DEBUG("[KeeperRecordingClient] KeeperRecordingiClient Constructor for {}", to_string(keeper_service_id));
        std::string service_addr_string;
        recordingServiceId.get_service_as_string(service_addr_string);

        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), recordingServiceId.getProviderId());

        tail_get_sequences = procedures_->calls[ClientRpcProcedures::TailSequences];
        tail_get_events = procedures_->calls[ClientRpcProcedures::TailEvents];
    }


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const&) = delete;
    KeeperRecordingClient& operator=(KeeperRecordingClient const&) = delete;
};
} // namespace chronolog

#endif
