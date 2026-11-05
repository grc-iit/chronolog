#include "ClientQueryService.h"
#include "PlaybackQueryRpcClient.h"
#include <PlaybackQueryResponse.h>
#include <chrono_monitor.h>
#include <private/RpcBulkSerialization.h>
#include <thread>
#include <exception>
#include <limits>
namespace chl = chronolog;
namespace tl = thallium;
namespace chronolog
{
namespace
{
struct QueryConsumption
{
    std::shared_ptr<PlaybackQuery> query;
    ClientQueryService* service;
    void (*cleanup)(ClientQueryService*, uint32_t);
    ~QueryConsumption()
    {
        query->closeCallbacks();
        query->consumed.store(true, std::memory_order_release);
        cleanup(service, query->queryId);
    }
};
struct QueryOwnership
{
    std::shared_ptr<ReplayEntry> entry;
    rpc::BulkPermits::Permit permit;
    QueryOwnership(std::shared_ptr<ReplayEntry> retained, rpc::BulkPermits::Permit admitted)
        : entry(std::move(retained))
        , permit(std::move(admitted))
    {}
};
class SubmitQueryWork final: public rpc::OwnedRpcWork<int>
{
public:
    SubmitQueryWork(std::shared_ptr<ClientRpcContext> owner,
                    std::shared_ptr<PlaybackQuery> query,
                    std::shared_ptr<PlaybackQueryRpcClient> route,
                    ServiceId receiver,
                    uint64_t attempt)
        : owner_(std::move(owner))
        , query_(std::move(query))
        , route_(std::move(route))
        , receiver_(std::move(receiver))
        , attempt_(attempt)
    {}
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        auto result = route_->send_story_playback_request(*owner_, operation, native_, receiver_, *query_, attempt_);
        int status = result.reply ? (*result.reply == query_->queryId ? CL_SUCCESS : CL_ERR_NO_PLAYERS) : result.status;
        query_->submissionStatus = status;
        query_->submissionKnown = result.reply.has_value() || !result.may_have_dispatched;
        query_->submissionReady.store(true, std::memory_order_release);
        if(result.reply || !result.may_have_dispatched)
            return {CL_SUCCESS, result.may_have_dispatched, status};
        return {result.status, true, {}};
    }

private:
    std::shared_ptr<ClientRpcContext> owner_;
    std::shared_ptr<PlaybackQuery> query_;
    std::shared_ptr<PlaybackQueryRpcClient> route_;
    ServiceId receiver_;
    uint64_t attempt_;
    rpc::RpcCallState native_; // Native fields die before their actual owner.
};
void pause(rpc::RpcDeadline const& deadline)
{
    auto remaining = deadline.remaining();
    auto interval = std::chrono::duration_cast<rpc::RpcDeadline::Clock::duration>(std::chrono::milliseconds(1));
    if(remaining > rpc::RpcDeadline::Clock::duration::zero())
        std::this_thread::sleep_for(std::min(remaining, interval));
}
} // namespace
void PlaybackQuery::closeCallbacks() noexcept
{
    callbackState.fetch_or(CallbackClosed, std::memory_order_acq_rel);
    // This wait covers only an already-admitted cooperative user callback.
    // Native work, diagnostics and map locks are outside the active interval.
    while(callbackState.load(std::memory_order_acquire) & CallbackActive)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    callback = {}; // Release user captures before the public replay returns.
}
ClientQueryService* ClientQueryService::CreateClientQueryService(thallium::engine& engine,
                                                                 ServiceId const& id,
                                                                 std::shared_ptr<ClientRpcProcedures> procedures,
                                                                 std::weak_ptr<ClientRpcContext> context)
{
    auto owner = context.lock();
    if(!owner || owner->closing() || !procedures || !owner->engine ||
       owner->engine->get_margo_instance() != engine.get_margo_instance() || owner->reader)
        return nullptr;
    auto* service = new ClientQueryService(engine, id, std::move(procedures), owner);
    owner->reader = service; // Actual root precedes every callback registration.
    service->registerProcedures(owner->readerRegistrationObserver_);
    return service;
}
ClientQueryService::ClientQueryService(thallium::engine& engine,
                                       ServiceId const& id,
                                       std::shared_ptr<ClientRpcProcedures> procedures,
                                       std::weak_ptr<ClientRpcContext> context)
    : tl::provider<ClientQueryService>(engine, id.getProviderId())
    , procedures_(std::move(procedures))
    , context_(std::move(context))
    , queryServiceEngine(engine)
    , queryServiceId(id)
{}
void ClientQueryService::registerProcedures(std::function<void(ClientQueryService&, bool)> const& observer)
{
    if(observer)
        observer(*this, false);
    define("receive_query_response", &ClientQueryService::receive_query_response, tl::ignore_return_value());
    if(observer)
        observer(*this, true);
}
ClientQueryService::~ClientQueryService() = default;
int ClientQueryService::addStoryReader(ChronicleName const& chronicle,
                                       StoryName const& story,
                                       StoryId const& id,
                                       ServiceId const& service)
{
    auto client = addPlaybackQueryClient(service);
    if(!client)
        return CL_ERR_UNKNOWN;
    std::lock_guard<std::timed_mutex> lock(queryServiceMutex);
    return acquiredStoryMap.emplace(std::make_pair(chronicle, story), std::make_pair(id, client)).second
                   ? CL_SUCCESS
                   : CL_ERR_UNKNOWN;
}
void ClientQueryService::removeStoryReader(ChronicleName const& chronicle, StoryName const& story)
{
    std::lock_guard<std::timed_mutex> lock(queryServiceMutex);
    acquiredStoryMap.erase({chronicle, story});
}
void ClientQueryService::stop_query(uint32_t query)
{
    // Expiry has no extra cleanup budget. If another map operation owns this
    // lock, the consumed record stays charged until the next replay prunes it.
    std::unique_lock<std::timed_mutex> lock(queryServiceMutex, std::try_to_lock);
    if(lock.owns_lock())
        activeQueryMap.erase(query);
}
bool ClientQueryService::pruneConsumedQueries() noexcept
{
    std::unique_lock<std::timed_mutex> lock(queryServiceMutex, std::try_to_lock);
    if(!lock.owns_lock())
        return false;
    for(auto found = activeQueryMap.begin(); found != activeQueryMap.end();)
        if(found->second->consumed.load(std::memory_order_acquire))
            found = activeQueryMap.erase(found);
        else
            ++found;
    return activeQueryMap.empty();
}
int ClientQueryService::dispatch_query(std::shared_ptr<PlaybackQuery> const& query,
                                       std::shared_ptr<PlaybackQueryRpcClient> const& route,
                                       std::shared_ptr<ClientRpcContext> const& owner,
                                       rpc::RpcDeadline const& deadline,
                                       uint64_t attemptMs)
{
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return CL_ERR_RPC_CAPACITY;
    auto control = rpc::RpcDeadline::after(std::chrono::milliseconds(attemptMs));
    if(deadline.expiresAt() < control.expiresAt())
        control = deadline;
    auto reserved = runtime->reserve(control);
    if(!reserved)
        return reserved.status() == CL_ERR_DEADLINE_EXCEEDED ? CL_ERR_QUERY_TIMED_OUT : reserved.status();
    auto pending =
            runtime->submit<int>(std::move(reserved),
                                 std::make_shared<SubmitQueryWork>(owner, query, route, queryServiceId, attemptMs));
    bool observedSubmission = false;
    for(;;)
    {
        if(deadline.expired())
        {
            query->phase.store(PlaybackQuery::Abandoned, std::memory_order_release);
            return CL_ERR_QUERY_TIMED_OUT;
        }
        auto expected = PlaybackQuery::Complete;
        if(query->phase.compare_exchange_strong(expected, PlaybackQuery::Consuming, std::memory_order_acq_rel))
            return query->complete ? CL_SUCCESS : CL_ERR_PARTIAL_RESULT;
        if(expected == PlaybackQuery::Abandoned)
            return CL_ERR_QUERY_TIMED_OUT;
        if(!observedSubmission && query->submissionReady.load(std::memory_order_acquire) && query->submissionKnown &&
           query->submissionStatus != CL_ERR_DEADLINE_EXCEEDED)
        {
            observedSubmission = true;
            pending = {}; // Settled native/local replies can release the slot.
            if(query->submissionStatus != CL_SUCCESS)
                return CL_ERR_NO_PLAYERS;
        }
        else if(!observedSubmission && control.expired())
        {
            // This wait is already expired. It independently abandons queued
            // native work without holding up a completed private response.
            auto submission = pending.wait();
            observedSubmission = true;
            if(submission.reply && *submission.reply != CL_SUCCESS)
                return *submission.reply == CL_ERR_DEADLINE_EXCEEDED ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_NO_PLAYERS;
            if(!submission.may_have_dispatched && !submission.reply)
                return submission.status == CL_ERR_RPC_CAPACITY ? CL_ERR_RPC_CAPACITY : CL_ERR_QUERY_TIMED_OUT;
        }
        // An unknown scalar ACK does not reject a response already in flight.
        // Continue under the original replay expiry, never another submit.
        pause(deadline);
    }
}
int ClientQueryService::replay(ChronicleName const& chronicle,
                               StoryName const& story,
                               uint64_t start,
                               uint64_t end,
                               std::vector<Event>* output,
                               Client::EventCallback callback,
                               rpc::RpcDeadline const& deadline,
                               std::shared_ptr<ReplayEntry> entry)
{
    if(!output && !callback)
        return CL_ERR_INVALID_ARG;
    if(!entry)
        return CL_ERR_RPC_CAPACITY;
    auto owner = context_.lock();
    if(!owner)
        return CL_ERR_RPC_CAPACITY;
    std::shared_ptr<PlaybackQuery> query;
    std::shared_ptr<PlaybackQueryRpcClient> route;
    {
        std::unique_lock<std::timed_mutex> lock(queryServiceMutex, std::defer_lock);
        if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
            return CL_ERR_QUERY_TIMED_OUT;
        for(auto found = activeQueryMap.begin(); found != activeQueryMap.end();)
            if(found->second->consumed.load(std::memory_order_acquire))
                found = activeQueryMap.erase(found);
            else
                ++found;
        auto found = acquiredStoryMap.find({chronicle, story});
        if(found == acquiredStoryMap.end())
            return CL_ERR_NOT_ACQUIRED;
        auto permit = owner->replayQueries.acquire();
        // Keep the signed error half of the uint32 ACK space disjoint from IDs.
        // IDs survive native context replacement, including erased/expired queries.
        if(!permit)
            return CL_ERR_RPC_CAPACITY;
        auto index = queryIndex.load(std::memory_order_acquire);
        for(;;)
        {
            if(index >= uint64_t(std::numeric_limits<int32_t>::max()))
                return CL_ERR_RPC_CAPACITY;
            if(queryIndex.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel))
                break;
        }
        auto retained = std::make_shared<QueryOwnership>(entry, std::move(*permit));
        query = std::make_shared<PlaybackQuery>(static_cast<uint32_t>(index + 1),
                                                deadline.expiresAt(),
                                                found->second.first,
                                                chronicle,
                                                story,
                                                start,
                                                end,
                                                std::move(callback));
        query->retention = std::move(retained);
        route = found->second.second;
        activeQueryMap.emplace(query->queryId, query);
    }
    // This guard cannot allocate after the query has been published.
    QueryConsumption handoff{query, this, [](ClientQueryService* service, uint32_t id) { service->stop_query(id); }};
    int status = dispatch_query(query, route, owner, deadline, entry->policy.policy().attempt_ms);
    if((status == CL_SUCCESS || status == CL_ERR_PARTIAL_RESULT) && output)
    {
        if(deadline.expired())
            return CL_ERR_QUERY_TIMED_OUT;
        *output = std::move(query->events);
        query->discard = false;
    }
    if(status != CL_SUCCESS && status != CL_ERR_PARTIAL_RESULT)
        query->phase.store(PlaybackQuery::Abandoned, std::memory_order_release);
    return status;
}
int ClientQueryService::replay_story(ChronicleName const& chronicle,
                                     StoryName const& story,
                                     uint64_t start,
                                     uint64_t end,
                                     std::vector<Event>& output,
                                     rpc::RpcDeadline const& deadline,
                                     std::shared_ptr<ReplayEntry> entry)
{
    return replay(chronicle, story, start, end, &output, {}, deadline, std::move(entry));
}
int ClientQueryService::replay_story(ChronicleName const& chronicle,
                                     StoryName const& story,
                                     uint64_t start,
                                     uint64_t end,
                                     Client::EventCallback callback,
                                     rpc::RpcDeadline const& deadline,
                                     std::shared_ptr<ReplayEntry> entry)
{
    return replay(chronicle, story, start, end, nullptr, std::move(callback), deadline, std::move(entry));
}
std::shared_ptr<PlaybackQueryRpcClient> ClientQueryService::addPlaybackQueryClient(ServiceId const& player)
{
    std::lock_guard<std::timed_mutex> lock(queryServiceMutex);
    auto found = playbackRpcClientMap.find(player.get_service_endpoint());
    if(found != playbackRpcClientMap.end())
        return found->second;
    auto candidate = PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(player);
    playbackRpcClientMap.emplace(player.get_service_endpoint(), candidate);
    return candidate;
}
void ClientQueryService::receive_query_response(tl::request const& request, tl::bulk& source)
{
    auto owner = context_.lock();
    auto native = owner ? owner->acquireNative() : std::optional<ClientRpcContext::NativeLease>{};
    if(!owner || !native)
        return;
    auto lease = owner->incomingResponses.acquire();
    if(!lease)
        return;
    struct NativeOwner
    {
        ClientRpcContext::NativeLease lease;
        explicit NativeOwner(ClientRpcContext::NativeLease retained)
            : lease(std::move(retained))
        {}
    };
    struct Receive: rpc::BulkReceiveContext
    {
        using rpc::BulkReceiveContext::BulkReceiveContext;
        PlaybackQueryResponse response;
        std::shared_ptr<PlaybackQuery> query;
    };
    std::shared_ptr<Receive> context;
    try
    {
        auto retained = std::make_shared<NativeOwner>(std::move(*native));
        context = std::make_shared<Receive>(retained,
                                            *owner->engine,
                                            request,
                                            source,
                                            owner->incomingResponses.observers);
        lease->retain(context);
        if(!context->pull())
        {
            lease->quarantine();
            return;
        }
        if(!rpc::deserializeBulk(context->bytes.data(), context->bytes.size(), context->response))
        {
            if(!context->answer(uint64_t{0}))
                lease->quarantine();
            return;
        }
        std::shared_ptr<PlaybackQuery> query;
        {
            std::lock_guard<std::timed_mutex> lock(queryServiceMutex);
            auto found = activeQueryMap.find(context->response.query_id);
            if(found != activeQueryMap.end())
                query = found->second;
        }
        context->query = query; // A quarantined incoming answer retains the query permit too.
        bool published = false, publishingOwner = false;
        if(query && std::chrono::steady_clock::now() < query->deadline)
        {
            auto expected = PlaybackQuery::Pending;
            if(query->phase.compare_exchange_strong(expected, PlaybackQuery::Publishing, std::memory_order_acq_rel))
            {
                publishingOwner = true;
                bool delivered = true;
                if(query->callbackMode)
                {
                    for(auto const& event: context->response.events)
                    {
                        if(!query->beginCallback())
                        {
                            delivered = false;
                            break;
                        }
                        std::exception_ptr failure;
                        try
                        {
                            query->callback(event);
                        }
                        catch(...)
                        {
                            failure = std::current_exception();
                        }
                        query->endCallback();
                        // Diagnostics stay in this charged receive context,
                        // after the user-callback interval is released.
                        if(failure)
                        {
                            try
                            {
                                std::rethrow_exception(failure);
                            }
                            catch(std::exception const& error)
                            {
                                LOG_ERROR("[ClientQueryService] Replay callback threw: {}", error.what());
                            }
                            catch(...)
                            {
                                LOG_ERROR("[ClientQueryService] Replay callback threw");
                            }
                        }
                    }
                }
                else
                    query->events = std::move(context->response.events);
                query->complete = context->response.complete;
                expected = PlaybackQuery::Publishing;
                if(delivered && std::chrono::steady_clock::now() < query->deadline)
                    published = query->phase.compare_exchange_strong(expected,
                                                                     PlaybackQuery::Complete,
                                                                     std::memory_order_acq_rel);
                else
                    query->phase.store(PlaybackQuery::Abandoned, std::memory_order_release);
            }
        }
        // No caller vector is reachable. The owned callback has closed/active
        // arbitration and never runs under the map lock.
        // An expired/unknown query is a known rejection after the full pull.
        if(!context->answer(published ? uint64_t(context->bytes.size()) : uint64_t{0}))
            lease->quarantine();
        else if(publishingOwner)
        {
            // Only the publishing winner owns staging. A rejected duplicate
            // must neither clear that storage nor wait on its consumer.
            // Reply first: the peer may withhold the original scalar ACK until
            // this response arrives. Keep incoming capacity charged while the
            // private decoded result awaits its public caller's handoff.
            bool discarded = false;
            while(!query->consumed.load(std::memory_order_acquire))
            {
                if(std::chrono::steady_clock::now() >= query->deadline)
                {
                    auto expected = PlaybackQuery::Complete;
                    if(query->phase.compare_exchange_strong(expected,
                                                            PlaybackQuery::Abandoned,
                                                            std::memory_order_acq_rel) ||
                       expected == PlaybackQuery::Abandoned)
                    {
                        discarded = true;
                        break;
                    }
                }
                margo_thread_sleep(owner->engine->get_margo_instance(), 1);
            }
            if(discarded || query->discard)
                std::vector<Event>{}.swap(query->events);
        }
    }
    catch(...)
    {
        if(context)
        {
            if(context->initiated && !context->completed)
            {
                lease->quarantine();
                return;
            }
            if(!context->answer(uint64_t{0}))
                lease->quarantine();
        }
    }
}
} // namespace chronolog
