#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <chronolog_types.h>

#include "KeeperRecordingClient.h"
#include "KeeperTailReader.h"
#include "StorytellerClient.h"
#include "PlaybackQueryRpcClient.h"

namespace tl = thallium;

namespace chl = chronolog;

/////////////////////

/////////////////////
chronolog::StoryHandle::~StoryHandle() {}

////////////////////
template <class KeeperChoicePolicy>
// = chronolog::RoundRobinKeeperChoice>
chronolog::StoryHandleImpl<KeeperChoicePolicy>::~StoryHandleImpl()
{
    delete keeperChoicePolicy;
}

////////////////////
template <class KeeperChoicePolicy>
void chronolog::StoryHandleImpl<KeeperChoicePolicy>::addRecordingClient(chronolog::KeeperRecordingClient* keeperClient)
{
    storyKeepers.push_back(keeperClient);
}

///////////////////
template <class KeeperChoicePolicy>
void chronolog::StoryHandleImpl<KeeperChoicePolicy>::removeRecordingClient(
        chronolog::ServiceId const& keeper_service_id)
{
    // this should only be called when the ChronoKeeper process unexpectedly exits
    // so it's ok to use rather inefficient vector iteration....
    for(auto iter = storyKeepers.begin(); iter != storyKeepers.end(); ++iter)
    {
        if((*iter)->getRecordingServiceId() == keeper_service_id)
        {
            storyKeepers.erase(iter);
            break;
        }
    }
}

template <class KeeperChoicePolicy>
class chronolog::StoryHandleImpl<KeeperChoicePolicy>::AppendWork final: public rpc::OwnedRpcWork<int>
{
public:
    AppendWork(std::shared_ptr<ClientRpcContext> owner,
               EntryLease entry,
               RpcPolicyContext::Snapshot policy,
               std::unique_ptr<KeeperRecordingClient::AppendRequest> request,
               StoryId story,
               uint64_t timestamp,
               ClientId client,
               chrono_index index,
               std::string const& record)
        : owner_(std::move(owner))
        , entry_(std::move(entry))
        , policy_(std::move(policy))
        , request_(std::move(request))
        , event_(story, timestamp, client, index, record)
    {}
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        return request_->send_event_msg(operation, event_, policy_.policy().attempt_ms);
    }

private:
    std::shared_ptr<ClientRpcContext> owner_;
    EntryLease entry_;
    RpcPolicyContext::Snapshot policy_;
    std::unique_ptr<KeeperRecordingClient::AppendRequest> request_;
    LogEvent event_;
};

template <class KeeperChoicePolicy>
uint64_t chronolog::StoryHandleImpl<KeeperChoicePolicy>::log_event(std::string const& record)
{
    auto result = appendFromEntry(record, rpc::RpcDeadline::Clock::now());
    return result.outcome == AppendOutcome::Acknowledged ? result.timestamp : 0;
}

template <class KeeperChoicePolicy>
chronolog::AppendResult chronolog::StoryHandleImpl<KeeperChoicePolicy>::append_event(std::string const& record)
{
    return appendFromEntry(record, rpc::RpcDeadline::Clock::now());
}

template <class KeeperChoicePolicy>
chronolog::AppendResult
chronolog::StoryHandleImpl<KeeperChoicePolicy>::appendFromEntry(std::string const& record,
                                                                rpc::RpcDeadline::Clock::time_point started)
{
    auto entry = tryEnter();
    if(!entry)
        return {CL_ERR_NOT_ACQUIRED, AppendOutcome::NotSubmitted, 0, 0};
    auto owner = context_.lock();
    auto native = owner ? owner->acquireNative() : std::optional<ClientRpcContext::NativeLease>{};
    if(!native)
        return {CL_ERR_RPC_CAPACITY, AppendOutcome::NotSubmitted, 0, 0};
    entry->retainNative(std::move(*native));
    auto policy = theClient.acquirePolicy();
    if(!policy)
        return {CL_ERR_RPC_CAPACITY, AppendOutcome::NotSubmitted, 0, 0};
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(policy->policy().append_ms));
    return append(record, deadline, std::move(*policy), std::move(*entry), std::move(owner));
}

template <class KeeperChoicePolicy>
chronolog::AppendResult chronolog::StoryHandleImpl<KeeperChoicePolicy>::append(std::string const& record,
                                                                               rpc::RpcDeadline const& deadline,
                                                                               RpcPolicyContext::Snapshot policy,
                                                                               EntryLease entry,
                                                                               std::shared_ptr<ClientRpcContext> owner)
{
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return {CL_ERR_RPC_CAPACITY, AppendOutcome::NotSubmitted, 0, 0};
    auto timestamp = theClient.getTimestamp(deadline.expiresAt());
    if(timestamp.status != CL_SUCCESS)
        return {timestamp.status, AppendOutcome::NotSubmitted, 0, 0};
    auto index = theClient.get_event_index();
    AppendResult result{CL_ERR_DEADLINE_EXCEEDED, AppendOutcome::NotSubmitted, timestamp.timestamp, index};
    if(deadline.expired())
        return result;
    // Clock failure/contending assignment needs no owned RPC context. Reserve
    // only after stamping, before allocating the owned descriptor or payload.
    auto reservation = runtime->reserve(deadline);
    if(!reservation)
    {
        result.status = reservation.status();
        return result;
    }
    if(storyKeepers.empty())
    {
        result.status = CL_ERR_NO_KEEPERS;
        return result;
    }
    auto keeper = keeperChoicePolicy->chooseKeeper(storyKeepers, timestamp.timestamp);
    if(!keeper)
    {
        result.status = CL_ERR_NO_KEEPERS;
        return result;
    }
    // Reserve before copying the payload or acquiring native-bearing descriptors.
    // Reserved submission transfers all ownership even when expiry/close wins.
    auto work = std::make_shared<AppendWork>(owner,
                                             std::move(entry),
                                             std::move(policy),
                                             keeper->prepareAppend(),
                                             storyId,
                                             timestamp.timestamp,
                                             theClient.getClientId(),
                                             index,
                                             record);
    auto reply = runtime->invoke<int>(std::move(reservation), std::move(work));
    result.status = reply.status;
    if(reply.reply)
    {
        result.status = *reply.reply;
        result.outcome = *reply.reply == CL_SUCCESS ? AppendOutcome::Acknowledged : AppendOutcome::Rejected;
    }
    else if(reply.may_have_dispatched)
    {
        result.status = CL_ERR_OUTCOME_UNCONFIRMED;
        result.outcome = AppendOutcome::Unconfirmed;
    }
    // One line per call the keeper did not answer, naming the keeper, as in
    // v3.4.0.
    if(!reply.reply)
        LOG_ERROR("[KeeperRecordingClient] Failed to send event message to {}: {}",
                  to_string(keeper->getRecordingServiceId()),
                  result.status);
    return result;
}

template <class KeeperChoicePolicy>
class chronolog::StoryHandleImpl<KeeperChoicePolicy>::DurableWork final: public rpc::OwnedRpcWork<int>
{
public:
    DurableWork(std::shared_ptr<ClientRpcContext> owner,
                EntryLease entry,
                RpcPolicyContext::Snapshot policy,
                std::unique_ptr<KeeperRecordingClient::DurableRequest> request,
                ChronicleName const& chronicle,
                StoryName const& story,
                LogEvent event,
                LogEventOptions const& options)
        : owner_(std::move(owner))
        , entry_(std::move(entry))
        , policy_(std::move(policy))
        , request_(std::move(request))
        , chronicle_(chronicle)
        , story_(story)
        , event_(std::move(event))
        , options_(options)
    {}
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        return request_->send(operation, chronicle_, story_, event_, options_.timeout_ms, options_.attempts);
    }

private:
    std::shared_ptr<ClientRpcContext> owner_;
    EntryLease entry_;
    RpcPolicyContext::Snapshot policy_;
    std::unique_ptr<KeeperRecordingClient::DurableRequest> request_;
    ChronicleName chronicle_;
    StoryName story_;
    LogEvent event_;
    LogEventOptions options_;
};

template <class KeeperChoicePolicy>
int chronolog::StoryHandleImpl<KeeperChoicePolicy>::log_event(std::string const& record,
                                                              LogEventOptions const& options,
                                                              uint64_t& event_time)
{
    auto started = rpc::RpcDeadline::Clock::now();
    event_time = 0;
    if(options.durability == Durability::Durable)
    {
        // The keeper drops an empty record after answering, which would make
        // a durable success false.
        if(options.attempts == 0 || options.timeout_ms == 0 || record.empty())
            return CL_ERR_INVALID_ARG;
        return logDurable(record, options, event_time, started);
    }
    if(options.durability != Durability::Accepted)
        return CL_ERR_INVALID_ARG;
    auto result = appendFromEntry(record, started);
    if(result.outcome != AppendOutcome::Acknowledged)
        return result.status;
    event_time = result.timestamp;
    return CL_SUCCESS;
}

template <class KeeperChoicePolicy>
int chronolog::StoryHandleImpl<KeeperChoicePolicy>::logDurable(std::string const& record,
                                                               LogEventOptions const& options,
                                                               uint64_t& event_time,
                                                               rpc::RpcDeadline::Clock::time_point started)
{
    auto entry = tryEnter();
    if(!entry)
        return CL_ERR_NOT_ACQUIRED;
    auto owner = context_.lock();
    auto native = owner ? owner->acquireNative() : std::optional<ClientRpcContext::NativeLease>{};
    if(!native)
        return CL_ERR_RPC_CAPACITY;
    entry->retainNative(std::move(*native));
    auto policy = theClient.acquirePolicy();
    if(!policy)
        return CL_ERR_RPC_CAPACITY;
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return CL_ERR_RPC_CAPACITY;
    // One budget for the whole write, as replay has its own: attempts *
    // timeout_ms from the call, capped where the clock would overflow.
    constexpr uint64_t maxBudget =
            std::chrono::duration_cast<std::chrono::milliseconds>(rpc::RpcDeadline::Clock::duration::max()).count();
    uint64_t budget = uint64_t(options.timeout_ms) * options.attempts;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(std::min(budget, maxBudget)));
    auto timestamp = theClient.getTimestamp(deadline.expiresAt());
    if(timestamp.status != CL_SUCCESS)
        return timestamp.status;
    // Built once: every attempt carries the same identity.
    LogEvent event(storyId, timestamp.timestamp, theClient.getClientId(), theClient.get_event_index(), record);
    if(deadline.expired())
        return CL_ERR_DEADLINE_EXCEEDED;
    auto reservation = runtime->reserve(deadline);
    if(!reservation)
        return reservation.status();
    if(storyKeepers.empty())
    {
        LOG_WARNING("[StoryHandleImpl] No keepers for Chronicle: {}, Story: {}", chronicle, story);
        return CL_ERR_NO_KEEPERS;
    }
    auto keeper = keeperChoicePolicy->chooseKeeper(storyKeepers, event.time());
    if(!keeper)
        return CL_ERR_NO_KEEPERS;
    auto work = std::make_shared<DurableWork>(owner,
                                              std::move(*entry),
                                              std::move(*policy),
                                              keeper->prepareDurable(),
                                              chronicle,
                                              story,
                                              std::move(event),
                                              options);
    auto reply = runtime->invoke<int>(std::move(reservation), std::move(work));
    if(reply.reply)
    {
        // The keeper answers CL_ERR_DURABILITY_UNKNOWN itself when its WAL
        // failed while writing the event and could not erase it.
        if(*reply.reply == CL_SUCCESS || *reply.reply == CL_ERR_DURABILITY_UNKNOWN)
            event_time = timestamp.timestamp;
        return *reply.reply;
    }
    LOG_ERROR("[KeeperRecordingClient] Failed to send durable event to {}: {}",
              to_string(keeper->getRecordingServiceId()),
              reply.may_have_dispatched ? CL_ERR_DURABILITY_UNKNOWN : reply.status);
    if(!reply.may_have_dispatched)
        return reply.status;
    // An attempt went unanswered. A new log_event call would give the event a
    // new identity, so the caller gets the timestamp to look for it later.
    event_time = timestamp.timestamp;
    return CL_ERR_DURABILITY_UNKNOWN;
}

/////////////////////

// Tail read: delegate to the two-phase scatter/gather over this story's keepers.
template <class KeeperChoicePolicy>
int chronolog::StoryHandleImpl<KeeperChoicePolicy>::playback(size_t n, std::vector<Event>& events)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto entry = tryEnter();
    if(!entry)
        return CL_ERR_NOT_ACQUIRED;
    auto owner = context_.lock();
    auto native = owner ? owner->acquireNative() : std::optional<ClientRpcContext::NativeLease>{};
    if(!native)
        return CL_ERR_RPC_CAPACITY;
    entry->retainNative(std::move(*native));
    auto policy = theClient.acquirePolicy();
    if(!policy)
        return CL_ERR_RPC_CAPACITY;
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return CL_ERR_RPC_CAPACITY;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(policy->policy().tail_ms));
    struct Retention
    {
        std::shared_ptr<ClientRpcContext> owner;
        EntryLease entry;
        RpcPolicyContext::Snapshot policy;
    };
    auto retained = std::make_shared<Retention>(Retention{owner, std::move(*entry), std::move(*policy)});
    return gather_story_tail(storyKeepers,
                             storyId,
                             n,
                             events,
                             deadline,
                             *runtime,
                             retained,
                             retained->policy.policy().attempt_ms);
}
/////////////////////

chronolog::StorytellerClient::~StorytellerClient()
{
    clearAcquiredStoryHandles();
    for(auto const& keeper: recordingClientMap) delete keeper.second;
}
void chronolog::StorytellerClient::clearAcquiredStoryHandles()
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
    for(auto const& handle: acquiredStoryHandles)
        static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(handle.second)->invalidate();
    acquiredStoryHandles.clear();
    reapRetiredHandles();
}

void chronolog::StorytellerClient::reapRetiredHandles()
{
    for(auto position = ownedHandles_.begin(); position != ownedHandles_.end();)
        if(static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(position->get())->retiredAndIdle())
            position = ownedHandles_.erase(position);
        else
            ++position;
}
size_t chronolog::StorytellerClient::retainedHandleCount()
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
    reapRetiredHandles();
    return ownedHandles_.size();
}

void chronolog::StorytellerClient::quiesceNativeEntries()
{
    std::unique_lock<std::mutex> lock(acquiredStoryMapMutex);
    for(auto const& handle: ownedHandles_)
        static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(handle.get())->invalidate();
    acquiredStoryHandles.clear();
    for(;;)
    {
        bool settled = true;
        for(auto const& handle: ownedHandles_)
            settled &= static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(handle.get())->retiredAndIdle();
        if(settled)
            return;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}

chronolog::chrono_index chronolog::StorytellerClient::get_event_index() noexcept
{
    // Unsigned fetch_add wraps modulo 2^32; zero is a valid event index.
    // This is an identity component, not a gapless writer sequence.
    return atomic_index.fetch_add(1, std::memory_order_relaxed) + chrono_index{1};
}
////////////////


int chronolog::StorytellerClient::addKeeperRecordingClient(chronolog::ServiceId const& keeper_service_id)
{
    std::lock_guard<std::mutex> lock(recordingClientMapMutex);

    try
    {
        chronolog::KeeperRecordingClient* keeperRecordingClient =
                chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(client_engine,
                                                                              keeper_service_id,
                                                                              procedures_);

        if(!keeperRecordingClient)
            return 0;
        auto insert_return =
                recordingClientMap.insert(std::pair<std::pair<uint32_t, uint16_t>, chronolog::KeeperRecordingClient*>(
                        keeper_service_id.get_service_endpoint(),
                        keeperRecordingClient));
        if(false == insert_return.second)
        {
            delete keeperRecordingClient;
            LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}",
                      to_string(keeper_service_id));
            return 0;
        }
        LOG_INFO("[StorytellerClient] Added KeeperRecordingClient for {}", to_string(keeper_service_id));
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}", to_string(keeper_service_id));
    }

    // state = RUNNING;
    LOG_INFO("[StorytellerClient] RUNNING with {} KeeperRecordingClients", recordingClientMap.size());
    return 1;
}
/////////////////

int chronolog::StorytellerClient::removeKeeperRecordingClient(chronolog::ServiceId const& keeper_service_id)
{
    std::lock_guard<std::mutex> lock(recordingClientMapMutex);

    // stop & delete keeperRecordingClient before erasing keeper_process entry
    auto keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

    if(keeper_client_iter != recordingClientMap.end())
    {
        delete(*keeper_client_iter).second;
        recordingClientMap.erase(keeper_client_iter);
    }

    //INNA: TODO: if this function is triggered by the Vizor calls when the ChronoKeeper process unexpectedly unregistered/exited
    // we need to iterate through the known WritingHandles and make sure this keeperClient is removed from all the active storyHandles
    // serialize the log events by switching the state to PENDING and forcing the log event calls to wait by locking
    //recording clientMutex during this time ....
    LOG_INFO("[StorytellerClient] Removed KeeperRecordingClient for {}", to_string(keeper_service_id));
    return 1;
}

///////////////////////////
chronolog::StoryHandle* chronolog::StorytellerClient::findStoryHandle(ChronicleName const& chronicle,
                                                                      StoryName const& story)
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
    reapRetiredHandles();

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_DEBUG("[StorytellerClient::findStoryHandle] Found StoryHandle for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        return ((*story_record_iter).second);
    }
    else
    {
        LOG_DEBUG("[StorytellerClient::findStoryHandle] StoryHandle not found for Chronicle: '{}' and Story: "
                  "'{}'.",
                  chronicle,
                  story);
        return (nullptr);
    }
}

/////////////

chronolog::StoryHandle*
chronolog::StorytellerClient::initializeStoryHandle(ChronicleName const& chronicle,
                                                    StoryName const& story,
                                                    StoryId const& story_id,
                                                    std::vector<ServiceId> const& vectorOfKeepers,
                                                    chl::ServiceId const& player_card)
//INNA: TODO :KeeperChoicePolicy will have to be communicated here as well ....
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
    reapRetiredHandles();

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_DEBUG("[StorytellerClient] StoryHandle already exists for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        return story_record_iter->second;
    }

    // create new StoryHandleImpl & initialize its keeperClients vector
    auto ownedHandle = std::make_unique<StoryHandleImpl<RoundRobinKeeperChoice>>(*this, chronicle, story, story_id);
    auto* storyHandle = ownedHandle.get();

    for(ServiceId const& keeper_service_id: vectorOfKeepers)
    {
        auto keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

        if(keeper_client_iter == recordingClientMap.end())
        {
            // unlikely but we better check
            if(addKeeperRecordingClient(keeper_service_id) == 0)
            {
                LOG_WARNING("[StorytellerClient] Failed to add KeeperRecordingClient for {}",
                            to_string(keeper_service_id));
                return nullptr;
            }
        }
        keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

        storyHandle->addRecordingClient((*keeper_client_iter).second);
    }

    ownedHandles_.push_back(std::move(ownedHandle));
    auto insert_return =
            acquiredStoryHandles.insert(std::pair<std::pair<std::string, std::string>, chronolog::StoryHandle*>(
                    std::pair<std::string, std::string>(chronicle, story),
                    storyHandle));
    if(!insert_return.second)
    {
        LOG_ERROR("[StorytellerClient] Failed to insert StoryHandleImpl for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        ownedHandles_.pop_back();
        return nullptr;
    }

    LOG_INFO("[StorytellerClient] Successfully initialized StoryHandleImpl for Chronicle: '{}' and Story: '{}'.",
             chronicle,
             story);
    return storyHandle;
    /*
    // now check the state of the handle:
    // it's possible the other thread is still pending the acquisition response from the Vizor,
    // or the handle's keeper vector is being updated , etc ....
    if (state == PENDING_RESPONSE || state== UPDATING_KEEPERS) )
    {
    // get the handle lock and wait for the thread that sent the request to Vizor to get the response
        std::lock_guard<std::mutex> story_lock(storyHandleMutex);

    }
    */
}

//////////////////////
void chronolog::StorytellerClient::removeAcquiredStoryHandle(ChronicleName const& chronicle, StoryName const& story)
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
    reapRetiredHandles();

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        static_cast<StoryHandleImpl<RoundRobinKeeperChoice>*>(story_record_iter->second)->invalidate();
        // Retain admitted calls/owned continuations through settlement. Raw
        // pointer invocation must already satisfy the caller lifetime contract.
        // B05/B08 retain these leases in work that outlives its public call.
        acquiredStoryHandles.erase(story_record_iter);
        reapRetiredHandles();
        LOG_INFO("[StorytellerClient] Successfully removed StoryHandle for Chronicle: '{}' and Story: '{}'.",
                 chronicle,
                 story);
    }
    else
    {
        LOG_WARNING("[StorytellerClient] No matching StoryHandle found for Chronicle: '{}' and Story: '{}'.",
                    chronicle,
                    story);
    }
}

/////////////////

// instantiates every member, including removeRecordingClient, which nothing in
// the library calls yet; the unit tests do
template class chronolog::StoryHandleImpl<chronolog::RoundRobinKeeperChoice>;
