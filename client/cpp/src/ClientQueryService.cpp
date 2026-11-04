#include "ClientQueryService.h"
#include "PlaybackQueryRpcClient.h"
#include <PlaybackQueryResponse.h>
#include <private/RpcBulkSerialization.h>
#include <thread>
#include <exception>
namespace chl = chronolog;
namespace tl = thallium;
namespace chronolog
{
namespace
{
struct QueryConsumption
{
    std::shared_ptr<PlaybackQuery> query;
    ~QueryConsumption()
    {
        query->closeCallbacks();
        query->consumed.store(true, std::memory_order_release);
    }
};
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
    , queryIndex(0)
    , queryTimeoutInSecs(180)
{}
void ClientQueryService::registerProcedures(std::function<void(ClientQueryService&, bool)> const& observer)
{
    if(observer)
        observer(*this, false);
    define("receive_query_response", &ClientQueryService::receive_query_response, tl::ignore_return_value());
    if(observer)
        observer(*this, true);
}
ClientQueryService::~ClientQueryService()
{
    for(auto& client: playbackRpcClientMap) delete client.second;
}
int ClientQueryService::addStoryReader(ChronicleName const& chronicle,
                                       StoryName const& story,
                                       StoryId const& id,
                                       ServiceId const& service)
{
    auto* client = addPlaybackQueryClient(service);
    if(!client)
        return CL_ERR_UNKNOWN;
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    return acquiredStoryMap.emplace(std::make_pair(chronicle, story), std::make_pair(id, client)).second
                   ? CL_SUCCESS
                   : CL_ERR_UNKNOWN;
}
void ClientQueryService::removeStoryReader(ChronicleName const& chronicle, StoryName const& story)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    acquiredStoryMap.erase({chronicle, story});
}
std::shared_ptr<PlaybackQuery> ClientQueryService::start_query(std::chrono::steady_clock::time_point timeout,
                                                               StoryId const& story,
                                                               ChronicleName const& chronicle,
                                                               StoryName const& name,
                                                               chrono_time const& start,
                                                               chrono_time const& end,
                                                               Client::EventCallback callback)
{
    auto query = std::make_shared<PlaybackQuery>(++queryIndex,
                                                 timeout,
                                                 story,
                                                 chronicle,
                                                 name,
                                                 start,
                                                 end,
                                                 std::move(callback));
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    return activeQueryMap.emplace(query->queryId, query).second ? query : nullptr;
}
void ClientQueryService::stop_query(uint32_t query)
{
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    activeQueryMap.erase(query);
}
int ClientQueryService::dispatch_query(std::shared_ptr<PlaybackQuery> const& query, PlaybackQueryRpcClient* client)
{
    bool submitted = false;
    try
    {
        submitted = client && client->send_story_playback_request(query->queryId,
                                                                  query->storyId,
                                                                  query->chronicleName,
                                                                  query->storyName,
                                                                  query->startTime,
                                                                  query->endTime) == CL_SUCCESS;
    }
    catch(...)
    {}
    if(!submitted)
    {
        query->phase.store(PlaybackQuery::Abandoned, std::memory_order_release);
        query->closeCallbacks();
        stop_query(query->queryId);
        return CL_ERR_NO_PLAYERS;
    }
    int status = CL_ERR_QUERY_TIMED_OUT;
    for(;;)
    {
        if(std::chrono::steady_clock::now() >= query->deadline)
        {
            query->phase.store(PlaybackQuery::Abandoned, std::memory_order_release);
            break;
        }
        auto expected = PlaybackQuery::Complete;
        if(query->phase.compare_exchange_strong(expected, PlaybackQuery::Consuming, std::memory_order_acq_rel))
        {
            status = query->complete ? CL_SUCCESS : CL_ERR_PARTIAL_RESULT;
            break;
        }
        if(expected == PlaybackQuery::Abandoned)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    query->closeCallbacks();
    stop_query(query->queryId);
    return status;
}
int ClientQueryService::replay_story(ChronicleName const& chronicle,
                                     StoryName const& story,
                                     uint64_t start,
                                     uint64_t end,
                                     std::vector<Event>& output)
{
    auto started = std::chrono::steady_clock::now();
    std::pair<StoryId, PlaybackQueryRpcClient*> reader;
    {
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        auto found = acquiredStoryMap.find({chronicle, story});
        if(found == acquiredStoryMap.end())
            return CL_ERR_NOT_ACQUIRED;
        reader = found->second;
    }
    auto timeout = started + std::chrono::seconds(queryTimeoutInSecs);
    auto query = start_query(timeout, reader.first, chronicle, story, start, end);
    if(!query)
        return CL_ERR_RPC_CAPACITY;
    QueryConsumption handoff{query};
    int status = dispatch_query(query, reader.second);
    if(status == CL_SUCCESS || status == CL_ERR_PARTIAL_RESULT)
    {
        output = std::move(query->events);
        query->discard = false;
    }
    return status;
}
int ClientQueryService::replay_story(ChronicleName const& chronicle,
                                     StoryName const& story,
                                     uint64_t start,
                                     uint64_t end,
                                     Client::EventCallback callback)
{
    if(!callback)
        return CL_ERR_INVALID_ARG;
    auto started = std::chrono::steady_clock::now();
    std::pair<StoryId, PlaybackQueryRpcClient*> reader;
    {
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        auto found = acquiredStoryMap.find({chronicle, story});
        if(found == acquiredStoryMap.end())
            return CL_ERR_NOT_ACQUIRED;
        reader = found->second;
    }
    auto timeout = started + std::chrono::seconds(queryTimeoutInSecs);
    auto query = start_query(timeout, reader.first, chronicle, story, start, end, std::move(callback));
    if(!query)
        return CL_ERR_RPC_CAPACITY;
    QueryConsumption handoff{query};
    return dispatch_query(query, reader.second);
}
PlaybackQueryRpcClient* ClientQueryService::addPlaybackQueryClient(ServiceId const& player)
{
    {
        std::lock_guard<std::mutex> lock(queryServiceMutex);
        auto found = playbackRpcClientMap.find(player.get_service_endpoint());
        if(found != playbackRpcClientMap.end())
            return found->second;
    }
    // Native lookup is outside the query-map lock. B09 owns its independently
    // bounded control adapter; this task does not claim its conversion.
    auto candidate = std::unique_ptr<PlaybackQueryRpcClient>(
            PlaybackQueryRpcClient::CreatePlaybackQueryRpcClient(*this, player));
    if(!candidate)
        return nullptr;
    std::lock_guard<std::mutex> lock(queryServiceMutex);
    auto inserted = playbackRpcClientMap.emplace(player.get_service_endpoint(), candidate.get());
    if(inserted.second)
        candidate.release();
    return inserted.first->second;
}
void ClientQueryService::removePlaybackQueryClient(ServiceId const&)
{ /* Existing routes remain owned until reader cleanup. */ }
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
            std::lock_guard<std::mutex> lock(queryServiceMutex);
            auto found = activeQueryMap.find(context->response.query_id);
            if(found != activeQueryMap.end())
                query = found->second;
        }
        bool published = false;
        if(query && std::chrono::steady_clock::now() < query->deadline)
        {
            auto expected = PlaybackQuery::Pending;
            if(query->phase.compare_exchange_strong(expected, PlaybackQuery::Publishing, std::memory_order_acq_rel))
            {
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
        else if(query)
        {
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
