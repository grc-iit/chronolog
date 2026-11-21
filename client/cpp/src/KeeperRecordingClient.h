#ifndef KEEPER_RECORDING_CLIENT_H
#define KEEPER_RECORDING_CLIENT_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include "ClientRpcContext.h"
#include <RpcFanout.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
                // The timed forward has returned, so Mercury is done with it.
                // An unanswered append gives its slot back and still reports
                // CL_ERR_OUTCOME_UNCONFIRMED.
                operation.markNativeSettled();
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

    // A durable write: record_event_durable, sent again with the identical
    // event after an attempt that got no answer, up to `attempts` times, all
    // within the operation's deadline. The keeper stores an event identity at
    // most once, so the resend cannot create a second copy. It runs as one
    // owned job on one runtime slot, so a keeper that stopped answering costs
    // that slot, not one per attempt. Every attempt goes to this keeper: the
    // story's keeper list is read only by the caller, before submission.
    //
    // A keeper that lacks the RPC answers the forward with HG_NO_MATCH. That
    // is also HG_NOENTRY, which a forward to a dead keeper gets over
    // ofi+sockets. Once this keeper has answered record_event_durable it
    // defines the RPC, so HG_NO_MATCH from it is a transport error. Before
    // that, an answer to tail_get_sequences, which v3.3.0 keepers also
    // define, shows it is up and lacks the RPC; no answer means it is down
    // and the next attempt may reach it restarted at the same address.
    class DurableRequest
    {
    public:
        DurableRequest(tl::engine& engine,
                       ServiceId route,
                       std::shared_ptr<ClientRpcProcedures> procedures,
                       std::shared_ptr<std::atomic<bool>> answered)
            : engine_(engine)
            , route_(std::move(route))
            , procedures_(std::move(procedures))
            , answered_(std::move(answered))
        {}
        // A reply carries the final durable status. No reply with
        // may_have_dispatched set means an attempt went unanswered and nothing
        // later ruled out that it stored the event.
        rpc::RpcResult<int> send(rpc::OperationContext& operation,
                                 ChronicleName const& chronicle,
                                 StoryName const& story,
                                 LogEvent const& event,
                                 uint32_t timeoutMs,
                                 uint32_t attempts)
        {
            bool mayBeStored = false;
            for(uint32_t attempt = 1; attempt <= attempts; ++attempt)
            {
                auto started = rpc::RpcDeadline::Clock::now();
                int code = CL_ERR_UNKNOWN;
                switch(sendOnce(operation, chronicle, story, event, timeoutMs, code))
                {
                    case Outcome::Answered:
                        if(mayBeStored && (code == CL_ERR_DURABILITY_UNAVAILABLE || code == CL_ERR_DURABILITY_FULL))
                            code = CL_ERR_DURABILITY_UNKNOWN;
                        return {CL_SUCCESS, true, code};
                    case Outcome::NotRegistered:
                        // Nothing stored here, but an earlier unanswered
                        // attempt may have reached a keeper that stored it.
                        if(!mayBeStored)
                            return {CL_SUCCESS, true, CL_ERR_DURABILITY_UNAVAILABLE};
                        return {CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
                    case Outcome::NotSent:
                        if(mayBeStored)
                            return {CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
                        return {code, false, std::nullopt};
                    case Outcome::TimedOut:
                        mayBeStored = true;
                        break;
                    case Outcome::TransportError:
                        mayBeStored = true;
                        // A forward to a dead keeper fails at once over
                        // ofi+sockets. Each attempt takes timeoutMs, as one
                        // that times out does, so the attempts span the time
                        // a restarted keeper needs to come back.
                        if(attempt < attempts)
                            std::this_thread::sleep_until(std::min(started + std::chrono::milliseconds(timeoutMs),
                                                                   operation.deadline().expiresAt()));
                        break;
                }
                LOG_WARNING("[KeeperRecordingClient] Durable event attempt {} of {} to {} got no answer",
                            attempt,
                            attempts,
                            to_string(route_));
            }
            return {CL_ERR_OUTCOME_UNCONFIRMED, true, std::nullopt};
        }

    private:
        enum class Outcome
        {
            Answered,      // code holds the keeper's status
            NotRegistered, // the keeper is up and does not define record_event_durable
            NotSent,       // no forward left this client; code holds why
            TimedOut,      // no answer within the attempt's deadline
            TransportError // any other RPC failure after dispatch
        };
        Outcome sendOnce(rpc::OperationContext& operation,
                         ChronicleName const& chronicle,
                         StoryName const& story,
                         LogEvent const& event,
                         uint32_t timeoutMs,
                         int& code)
        {
            std::optional<std::chrono::milliseconds> timeout;
            try
            {
                if(operation.deadline().expired())
                {
                    code = CL_ERR_DEADLINE_EXCEEDED;
                    return Outcome::NotSent;
                }
                code = procedures_->provision(ClientRpcProcedures::RecordEventDurable,
                                              route_.getProviderId(),
                                              operation.deadline());
                if(code != CL_SUCCESS)
                    return Outcome::NotSent;
                if(!peer_)
                {
                    std::string address;
                    route_.get_service_as_string(address);
                    endpoint_.emplace(engine_.lookup(address));
                    peer_.emplace(*endpoint_, route_.getProviderId());
                }
                // Each attempt's callable is kept until the job is released.
                calls_.push_back(procedures_->calls[ClientRpcProcedures::RecordEventDurable].on(*peer_));
                timeout = operation.deadline().cappedAttempt(std::chrono::milliseconds(timeoutMs));
                if(!timeout || !operation.tryBeginDispatch())
                {
                    code = CL_ERR_DEADLINE_EXCEEDED;
                    return Outcome::NotSent;
                }
            }
            catch(...)
            {
                code = operation.deadline().expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_ERR_UNKNOWN;
                return Outcome::NotSent;
            }
            try
            {
                // Leaving this block, by return or exception, means the timed
                // forward has returned and Mercury is done with it.
                rpc::SettledOnExit settled{operation};
                code = calls_.back().timed(*timeout, chronicle, story, event);
                answered_->store(true, std::memory_order_release);
                return Outcome::Answered;
            }
            catch(tl::timeout const&)
            {
                return Outcome::TimedOut;
            }
            catch(tl::margo_exception const& ex)
            {
                if(ex.error() == HG_NO_MATCH && !answered_->load(std::memory_order_acquire) &&
                   keeperAnswers(operation, timeoutMs))
                {
                    LOG_ERROR("[KeeperRecordingClient] Keeper {} does not define record_event_durable",
                              to_string(route_));
                    return Outcome::NotRegistered;
                }
            }
            catch(...)
            {}
            return Outcome::TransportError;
        }
        // A tail read of no events of no story, which changes nothing on the
        // keeper. Waits at most the smaller of timeoutMs and 5 s, within the
        // operation's deadline.
        bool keeperAnswers(rpc::OperationContext& operation, uint32_t timeoutMs)
        {
            try
            {
                if(procedures_->provision(ClientRpcProcedures::TailSequences,
                                          route_.getProviderId(),
                                          operation.deadline()) != CL_SUCCESS)
                    return false;
                probes_.push_back(procedures_->calls[ClientRpcProcedures::TailSequences].on(*peer_));
                auto timeout = operation.deadline().cappedAttempt(
                        std::chrono::milliseconds(std::min<uint32_t>(timeoutMs, kAliveProbeMs)));
                if(!timeout || !operation.tryBeginDispatch())
                    return false;
                rpc::SettledOnExit settled{operation};
                std::vector<EventSequence> none = probes_.back().timed(*timeout, StoryId(0), size_t(0));
                return true;
            }
            catch(...)
            {}
            return false;
        }
        static constexpr uint32_t kAliveProbeMs = 5000;
        tl::engine& engine_;
        ServiceId route_;
        std::shared_ptr<ClientRpcProcedures> procedures_;
        std::shared_ptr<std::atomic<bool>> answered_;
        std::optional<tl::endpoint> endpoint_;
        std::optional<tl::provider_handle> peer_;
        using Call = decltype(std::declval<tl::remote_procedure>().on(std::declval<tl::provider_handle>()));
        // A deque never moves the calls already made.
        std::deque<Call> calls_;
        std::deque<Call> probes_;
    };
    std::unique_ptr<DurableRequest> prepareDurable()
    {
        return std::make_unique<DurableRequest>(engine_, recordingServiceId, procedures_, answeredDurable_);
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
                // Nothing between the issue and wait() throws, so leaving this
                // scope means wait() has returned and Mercury is done with it.
                rpc::SettledOnExit settled{operation};
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
                // settlement.
                timedOut_ = true;
                return {dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN, dispatched, std::nullopt};
            }
            catch(...)
            {
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
    // Set by the first answer to record_event_durable; never cleared.
    std::shared_ptr<std::atomic<bool>> answeredDurable_ = std::make_shared<std::atomic<bool>>(false);

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
