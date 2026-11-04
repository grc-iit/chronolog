#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <chrono>
#include "ClientRpcContext.h"
#include <RpcFanout.h>
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
                auto binding = procedures_->provision(ClientRpcProcedures::RecordEvent,
                                                      route_.getProviderId(),
                                                      operation.deadline());
                if(binding != CL_SUCCESS)
                    return {binding, false, std::nullopt};
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

    // Each retained child has at most one native request at a time. Lookup,
    // serialization, wait and native destruction all run on its charged worker.
    class TailRequest
    {
    public:
        TailRequest(tl::engine& engine, ServiceId route, std::shared_ptr<ClientRpcProcedures> procedures)
            : engine_(engine)
            , route_(std::move(route))
            , procedures_(std::move(procedures))
        {}
        rpc::RpcResult<std::vector<EventSequence>> sequences(rpc::OperationContext& operation,
                                                             StoryId story,
                                                             size_t n,
                                                             uint64_t cap,
                                                             rpc::FanoutIssueBarrier& issued)
        {
            return call<std::vector<EventSequence>>(operation,
                                                    ClientRpcProcedures::TailSequences,
                                                    cap,
                                                    issued,
                                                    story,
                                                    n);
        }
        rpc::RpcResult<std::vector<LogEvent>> events(rpc::OperationContext& operation,
                                                     StoryId story,
                                                     std::vector<EventSequence> const& sequences,
                                                     uint64_t cap,
                                                     rpc::FanoutIssueBarrier& issued)
        {
            return call<std::vector<LogEvent>>(operation,
                                               ClientRpcProcedures::TailEvents,
                                               cap,
                                               issued,
                                               story,
                                               sequences);
        }

        bool timedOut() const noexcept { return timedOut_; }

    private:
        template <class Reply, class... Args>
        rpc::RpcResult<Reply> call(rpc::OperationContext& operation,
                                   ClientRpcProcedures::Name name,
                                   uint64_t cap,
                                   rpc::FanoutIssueBarrier& issued,
                                   Args const&... args)
        {
            timedOut_ = false;
            bool dispatched = false, arrived = false;
            struct Arrival
            {
                rpc::FanoutIssueBarrier& barrier;
                rpc::RpcDeadline const& deadline;
                bool& arrived;
                ~Arrival()
                {
                    if(!arrived)
                        barrier.arriveAndWait(deadline);
                }
            } arrival{issued, operation.deadline(), arrived};
            try
            {
                if(operation.deadline().expired())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                auto binding = procedures_->provision(name, route_.getProviderId(), operation.deadline());
                if(binding != CL_SUCCESS)
                    return {binding, false, std::nullopt};
                if(!peer_)
                {
                    std::string address;
                    route_.get_service_as_string(address);
                    endpoint_.emplace(engine_.lookup(address));
                    peer_.emplace(*endpoint_, route_.getProviderId());
                }
                call_.emplace(procedures_->calls[name].on(*peer_));
                auto timeout = operation.deadline().cappedAttempt(std::chrono::milliseconds(cap));
                if(!timeout || !operation.tryBeginDispatch())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                dispatched = true;
                pending_.emplace(call_->timed_async(*timeout, args...));
                arrived = true;
                issued.arriveAndWait(operation.deadline());
                Reply reply = pending_->wait();
                pending_.reset();
                call_.reset();
                return {CL_SUCCESS, true, std::move(reply)};
            }
            catch(tl::timeout const&)
            {
                // Preserve the caller-visible cause independently of native
                // settlement. The unknown callable and slot remain retained.
                timedOut_ = true;
                return {dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN, dispatched, std::nullopt};
            }
            catch(...)
            {
                // Never destroy a possibly active request after a local timeout.
                return {dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN, dispatched, std::nullopt};
            }
        }
        tl::engine& engine_;
        ServiceId route_;
        std::shared_ptr<ClientRpcProcedures> procedures_;
        std::optional<tl::endpoint> endpoint_;
        std::optional<tl::provider_handle> peer_;
        using Call = decltype(std::declval<tl::remote_procedure>().on(std::declval<tl::provider_handle>()));
        std::optional<Call> call_;
        std::optional<tl::async_response> pending_;
        bool timedOut_ = false;
    };
    std::unique_ptr<TailRequest> prepareTail()
    {
        return std::make_unique<TailRequest>(engine_, recordingServiceId, procedures_);
    }

    ~KeeperRecordingClient() = default;

private:
    friend struct KeeperRecordingClientTestAccess;
    tl::engine& engine_;
    std::shared_ptr<AppendObservers const> appendObservers_;
    std::shared_ptr<ClientRpcProcedures> procedures_;
    ServiceId recordingServiceId;

    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    KeeperRecordingClient(tl::engine& tl_engine,
                          ServiceId const& keeper_service_id,
                          std::shared_ptr<ClientRpcProcedures> procedures)
        : engine_(tl_engine)
        , procedures_(std::move(procedures))
        , recordingServiceId(keeper_service_id)
    {}


    KeeperRecordingClient() = delete;
    KeeperRecordingClient(KeeperRecordingClient const&) = delete;
    KeeperRecordingClient& operator=(KeeperRecordingClient const&) = delete;
};
} // namespace chronolog

#endif
