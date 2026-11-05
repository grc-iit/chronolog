#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <thread>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ArchiveReadingRequestQueue.h>
#include <PlaybackService.h>
#include <PlayerDataStore.h>
#include <QueryResponseTransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::PlaybackService::PlaybackService(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                            uint16_t service_provider_id,
                                            chronolog::PlayerDataStore& activeDataStore,
                                            chronolog::ArchiveReadingRequestQueue& archive_reading_queue,
                                            RegistrationObserver observer)
    : tl::provider<PlaybackService>(*owner->engine, service_provider_id)
    , owner_(owner)
    , playbackEngine(*owner->engine)
    , theActiveDataStore(activeDataStore)
    , theArchiveReadingRequestQueue(archive_reading_queue)
    , registrationObserver_(std::move(observer))
{}

void chronolog::PlaybackService::registerProcedures()
{
    if(registrationObserver_)
        registrationObserver_(*this, false);
    define("playback_service_available", &PlaybackService::playback_service_available);
    if(registrationObserver_)
        registrationObserver_(*this, true);
    define("story_playback_request", &PlaybackService::story_playback_request);


    std::stringstream ss;
    ss << get_engine().self();
    LOG_INFO("[PlaybackService] Constructed at {}. ProviderID={}", ss.str(), get_provider_id());
}

////////////////////

chronolog::PlaybackService::~PlaybackService()
{
    LOG_DEBUG("[PlaybackService] Destructor called. Cleaning up...");

    {
        std::lock_guard<std::mutex> lock(hotFetchMutex);
        for(auto& client: hotFetchClients) { delete client.second; }
        hotFetchClients.clear();
    }

    std::lock_guard<std::mutex> lock(playbackServiceMutex);
    for(auto& agent: responseSenders)
    {
        agent.second->stopResponseThreads();
        delete agent.second;
    }
    responseSenders.clear();
}

//////////////////

chronolog::KeeperHotFetchClient* chronolog::PlaybackService::getHotFetchClient(chl::ServiceId const& keeper_service_id)
{
    {
        std::lock_guard<std::mutex> lock(hotFetchMutex);
        auto client_iter = hotFetchClients.find(keeper_service_id.get_service_endpoint());
        if(client_iter != hotFetchClients.end())
        {
            return client_iter->second;
        }
    }
    // engine lookup happens outside the cache mutex
    KeeperHotFetchClient* client = KeeperHotFetchClient::CreateKeeperHotFetchClient(
            owner_.lock(),
            keeper_service_id,
            std::chrono::milliseconds(owner_.lock()->policy.attempt_ms));
    if(client == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(hotFetchMutex);
    auto emplaced = hotFetchClients.emplace(keeper_service_id.get_service_endpoint(), client);
    if(!emplaced.second)
    {
        // another thread won the race; keep the cached one
        delete client;
    }
    return emplaced.first->second;
}

//////////////////


namespace chronolog
{
class PlaybackQueryWork final: public rpc::OwnedRpcWork<int>
{
public:
    PlaybackQueryWork(std::shared_ptr<PlaybackRpcOwner> owner,
                      PlaybackService* service,
                      rpc::RpcReservation child,
                      rpc::BulkPermits::Permit queryPermit,
                      rpc::BulkPermits::Permit sourcePermit,
                      tl::request const& request,
                      ServiceId receiver,
                      uint32_t query,
                      StoryId story,
                      std::string chronicle,
                      std::string name,
                      uint64_t start,
                      uint64_t end,
                      rpc::RpcDeadline ackDeadline)
        : queryPermit_(std::move(queryPermit))
        , sourcePermit_(std::move(sourcePermit))
        , owner_(std::move(owner))
        , service_(service)
        , child_(std::move(child))
        , receiver_(std::move(receiver))
        , query_(query)
        , story_(story)
        , chronicle_(std::move(chronicle))
        , name_(std::move(name))
        , start_(start)
        , end_(end)
        , ackDeadline_(ackDeadline)
        , request_(request)
    {}
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        if(ackDeadline_.expired() || !operation.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, 0};
        try
        {
            request_->respond(query_); // Exact v3 scalar ACK: query accepted.
            request_.reset();          // Only after a known native answer, on worker.
            auto* agent = service_->prepareQuery(receiver_,
                                                 query_,
                                                 story_,
                                                 chronicle_,
                                                 name_,
                                                 start_,
                                                 end_,
                                                 operation.deadline(),
                                                 response_);
            if(!agent)
                return {CL_ERR_RPC_CAPACITY, true, {}};
            while(!agent->takeReady(query_))
            {
                if(operation.deadline().expired())
                    return {CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto result = agent->transferOwnedQueryResponse(response_,
                                                            std::move(child_),
                                                            std::move(*sourcePermit_),
                                                            operation.deadline());
            sourcePermit_.reset();
            if(result.status == CL_SUCCESS)
                return {CL_SUCCESS, true, 1};
            if(!result.reply && result.may_have_dispatched)
            {
                // The child owns the immutable encoded response and source
                // permit. The coordinator needs no new slot to report expiry.
                response_.reset();
                return {CL_ERR_OUTCOME_UNCONFIRMED, true, 0};
            }
            // No confirmed delivery. Keep prepared events pending rather than
            // deleting them or claiming partial-success delivery.
            return {result.status, true, {}};
        }
        catch(...)
        {
            return {CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
        }
    }

private:
    rpc::BulkPermits::Permit queryPermit_;
    std::optional<rpc::BulkPermits::Permit> sourcePermit_;
    std::shared_ptr<PlaybackRpcOwner> owner_;
    PlaybackService* service_;
    rpc::RpcReservation child_;
    ServiceId receiver_;
    uint32_t query_;
    StoryId story_;
    std::string chronicle_, name_;
    uint64_t start_, end_;
    rpc::RpcDeadline ackDeadline_;
    std::shared_ptr<PlaybackQueryResponse> response_;
    std::optional<tl::request> request_;
};
} // namespace chronolog
void chronolog::PlaybackService::playback_service_available(tl::request const& request)
{
    auto owner = owner_.lock();
    auto entry = owner ? owner->enter() : std::optional<PlaybackRpcOwner::Entry>{};
    if(!entry)
        return;
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return;
    auto permit = owner->queryPermits.acquire();
    if(!permit)
        return;
    auto reserved = runtime->reserve(rpc::RpcDeadline::after(std::chrono::milliseconds(owner->policy.attempt_ms)));
    if(!reserved)
        return;
    class Ready final: public rpc::OwnedRpcWork<int>
    {
        rpc::BulkPermits::Permit permit_;
        std::shared_ptr<PlaybackRpcOwner> owner_;
        tl::request request_;

    public:
        Ready(rpc::BulkPermits::Permit permit, std::shared_ptr<PlaybackRpcOwner> owner, tl::request const& request)
            : permit_(std::move(permit))
            , owner_(std::move(owner))
            , request_(request)
        {}
        rpc::RpcResult<int> run(rpc::OperationContext& operation) override
        {
            if(!operation.tryBeginDispatch())
                return {CL_ERR_DEADLINE_EXCEEDED, false, 0};
            try
            {
                request_.respond(1);
                return {CL_SUCCESS, true, 1};
            }
            catch(...)
            {
                return {CL_ERR_OUTCOME_UNCONFIRMED, true, {}};
            }
        }
    };
    try
    {
        runtime->submit<int>(std::move(reserved), std::make_shared<Ready>(std::move(*permit), owner, request))
                .releaseToRuntime();
    }
    catch(...)
    {}
}
void chronolog::PlaybackService::story_playback_request(tl::request const& request,
                                                        ServiceId const& receiver,
                                                        uint32_t query,
                                                        StoryId const& story,
                                                        ChronicleName const& chronicle,
                                                        StoryName const& name,
                                                        chrono_time const& start,
                                                        chrono_time const& end)
{
    auto started = rpc::RpcDeadline::Clock::now();
    auto owner = owner_.lock();
    auto entry = owner ? owner->enter() : std::optional<PlaybackRpcOwner::Entry>{};
    if(!entry)
        return;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.replay_ms));
    auto admission = owner->reserveQuery(deadline);
    if(!admission)
        return;
    auto runtime = owner->runtime.lock();
    if(!runtime)
        return;
    auto ackDeadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(owner->policy.attempt_ms));
    try
    {
        auto work = std::make_shared<PlaybackQueryWork>(owner,
                                                        this,
                                                        std::move(admission->response),
                                                        std::move(admission->queryPermit),
                                                        std::move(admission->sourcePermit),
                                                        request,
                                                        receiver,
                                                        query,
                                                        story,
                                                        chronicle,
                                                        name,
                                                        start,
                                                        end,
                                                        ackDeadline);
        runtime->submit<int>(std::move(admission->coordinator), std::move(work)).releaseToRuntime();
    }
    catch(...)
    {}
}
chronolog::QueryResponseAgent*
chronolog::PlaybackService::prepareQuery(ServiceId const& receiver,
                                         uint32_t query,
                                         StoryId const& story,
                                         ChronicleName const& chronicle,
                                         StoryName const& name,
                                         chrono_time const& start,
                                         chrono_time const& end,
                                         rpc::RpcDeadline const& deadline,
                                         std::shared_ptr<PlaybackQueryResponse>& response)
{
    auto owner = owner_.lock();
    auto runtime = owner ? owner->runtime.lock() : nullptr;
    if(!owner || !runtime)
        return nullptr;
    QueryResponseAgent* agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(playbackServiceMutex);
        auto found = responseSenders.find(receiver.get_service_endpoint());
        if(found != responseSenders.end())
            agent = found->second;
        else
        {
            auto created =
                    std::unique_ptr<QueryResponseAgent>(QueryResponseAgent::CreateQueryResponseAgent(owner, receiver));
            if(!created)
                return nullptr;
            agent = created.get();
            responseSenders.emplace(receiver.get_service_endpoint(), created.release());
        }
    }
    auto keepers = theActiveDataStore.getStoryKeepers(story);
    auto hotDeadline = KeeperHotFetchClient::fanoutDeadline(deadline,
                                                            rpc::RpcDeadline::Clock::now(),
                                                            std::chrono::milliseconds(owner->policy.attempt_ms));
    std::vector<rpc::RpcReservation> reserved;
    std::vector<rpc::BulkPermits::Permit> hotPermits;
    bool admitted = keepers.size() <= 2;
    {
        std::unique_lock<std::timed_mutex> lock(owner->admissionMutex, std::defer_lock);
        if(!lock.try_lock_until(hotDeadline.expiresAt()))
            admitted = false;
        if(admitted)
            for(size_t i = 0; i < keepers.size(); ++i)
            {
                auto permit = owner->hotPermits.acquire();
                if(!permit)
                {
                    admitted = false;
                    break;
                }
                hotPermits.push_back(std::move(*permit));
                auto slot = runtime->reserve(hotDeadline);
                if(!slot)
                {
                    admitted = false;
                    break;
                }
                reserved.push_back(std::move(slot));
            }
    }
    if(!admitted)
    {
        reserved.clear();
        hotPermits.clear();
    }
    std::vector<std::pair<KeeperHotFetchClient*, rpc::RpcPending<HotRangeResponse>>> pending;
    auto barrier = std::make_shared<rpc::FanoutIssueBarrier>(keepers.size());
    bool allAnswered = admitted, anyTruncated = false;
    if(admitted)
        for(size_t i = 0; i < keepers.size(); ++i)
        {
            auto* client = getHotFetchClient(keepers[i]);
            if(!client)
            {
                allAnswered = false;
                continue;
            }
            pending.emplace_back(
                    client,
                    client->fetchRangeAsync(std::move(reserved[i]),
                                            story,
                                            start,
                                            end,
                                            262144,
                                            barrier,
                                            std::optional<rpc::BulkPermits::Permit>(std::move(hotPermits[i]))));
        }
    std::vector<HotRangeResponse> hot;
    for(auto& child: pending)
    {
        hot.push_back(child.first->waitForRange(child.second));
        allAnswered &= hot.back().answered;
        anyTruncated |= hot.back().truncated;
    }
    auto split = splitHotRange(hot, start, end);
    LOG_DEBUG("[PlaybackService] query_id {} story_id {} range {}-{} keepers {} hot_boundary {} hot_events {}",
              query,
              story,
              start,
              end,
              keepers.size(),
              split.boundary,
              split.hotEvents.size());
    auto plan = planReplay(split, start, end, allAnswered, anyTruncated);
    response = std::make_shared<PlaybackQueryResponse>(query);
    response->complete = plan.complete;
    for(auto const& event: split.hotEvents)
        response->events.push_back(Event{event.eventTime, event.clientId, event.eventIndex, event.logRecord});
    LOG_DEBUG("[PlaybackService] query {} for story_id {} got {} hot events from {} keeper(s)",
              query,
              story,
              response->events.size(),
              keepers.size());
    if(agent->stashOwnedQueryResponse(query, response, !plan.archiveNeeded) != CL_SUCCESS)
        return nullptr;
    if(plan.archiveNeeded)
    {
        auto request = std::make_unique<ArchiveReadingRequest>(agent, query, chronicle, name, start, plan.archiveEnd);
        theArchiveReadingRequestQueue.pushReadingRequest(request.get());
        request.release();
    }
    return agent;
}
