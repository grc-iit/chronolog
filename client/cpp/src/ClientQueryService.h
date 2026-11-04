#ifndef CLIENT_QUERY_SERVICE_H
#define CLIENT_QUERY_SERVICE_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <chronolog_types.h>
#include <ServiceId.h>
#include <chronolog_client.h>
#include "ClientRpcContext.h"

namespace tl = thallium;

namespace chronolog
{

class PlaybackQueryRpcClient;
struct PlaybackQueryResponse;

struct PlaybackQuery
{
    enum Phase
    {
        Pending,
        Publishing,
        Complete,
        Consuming,
        Abandoned
    };
    std::atomic<Phase> phase{Pending};
    std::vector<Event> events; // Private vector staging, never a caller reference.
    Client::EventCallback callback;
    bool const callbackMode;
    std::atomic<bool> consumed{false};
    bool discard = true; // Published by consumed; only the receive worker clears staging.
    static constexpr unsigned CallbackClosed = 1, CallbackActive = 2;
    std::atomic<unsigned> callbackState{0};
    bool beginCallback() noexcept
    {
        if(std::chrono::steady_clock::now() >= deadline)
            return false;
        unsigned expected = 0;
        if(!callbackState.compare_exchange_strong(expected, CallbackActive, std::memory_order_acq_rel))
            return false;
        if(std::chrono::steady_clock::now() < deadline)
            return true;
        callbackState.fetch_and(~CallbackActive, std::memory_order_acq_rel);
        return false;
    }
    void endCallback() noexcept { callbackState.fetch_and(~CallbackActive, std::memory_order_acq_rel); }
    void closeCallbacks() noexcept;
    bool complete = true;
    uint32_t queryId;
    std::chrono::steady_clock::time_point deadline;
    StoryId storyId;
    ChronicleName chronicleName;
    StoryName storyName;
    chrono_time startTime, endTime;
    PlaybackQuery(uint32_t query,
                  std::chrono::steady_clock::time_point timeout,
                  StoryId story,
                  ChronicleName chronicle,
                  StoryName name,
                  chrono_time start,
                  chrono_time end,
                  Client::EventCallback ownedCallback = {})
        : callback(std::move(ownedCallback))
        , callbackMode(bool(callback))
        , queryId(query)
        , deadline(timeout)
        , storyId(story)
        , chronicleName(std::move(chronicle))
        , storyName(std::move(name))
        , startTime(start)
        , endTime(end)
    {}
};

class ClientQueryService: public tl::provider<ClientQueryService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static ClientQueryService* CreateClientQueryService(thallium::engine&,
                                                        ServiceId const&,
                                                        std::shared_ptr<ClientRpcProcedures>,
                                                        std::weak_ptr<ClientRpcContext> context = {});

    ~ClientQueryService();

    std::shared_ptr<ClientRpcProcedures> procedures() const { return procedures_; }
    std::weak_ptr<ClientRpcContext> context() const noexcept { return context_; }

    tl::engine& get_service_engine() { return queryServiceEngine; }

    ServiceId const& get_service_id() const { return queryServiceId; }

    int addStoryReader(ChronicleName const&, StoryName const&, StoryId const&, ServiceId const&);
    void removeStoryReader(ChronicleName const&, StoryName const&);

    void receive_query_response(tl::request const&, tl::bulk&);

    int replay_story(ChronicleName const&,
                     StoryName const&,
                     uint64_t start,
                     uint64_t end,
                     std::vector<Event>& replay_events);

    int
    replay_story(ChronicleName const&, StoryName const&, uint64_t start, uint64_t end, Client::EventCallback callback);

private:
    friend struct ClientQueryServiceTestAccess;
    ClientQueryService(thallium::engine& tl_engine,
                       ServiceId const&,
                       std::shared_ptr<ClientRpcProcedures>,
                       std::weak_ptr<ClientRpcContext>);

    ClientQueryService() = delete;
    ClientQueryService(ClientQueryService const&) = delete;

    // create PlaybackServiceRpcClient associated with the remote Playback Service
    PlaybackQueryRpcClient* addPlaybackQueryClient(ServiceId const&);
    // destroy PlaybackServiceRpcClient associated with the remote Playback Service
    void removePlaybackQueryClient(ServiceId const&);

    std::shared_ptr<PlaybackQuery> start_query(std::chrono::steady_clock::time_point,
                                               StoryId const&,
                                               ChronicleName const&,
                                               StoryName const&,
                                               chrono_time const&,
                                               chrono_time const&,
                                               Client::EventCallback callback = {});
    void stop_query(uint32_t);
    int dispatch_query(std::shared_ptr<PlaybackQuery> const&, PlaybackQueryRpcClient*);
    void registerProcedures(std::function<void(ClientQueryService&, bool)> const& observer);
    std::shared_ptr<ClientRpcProcedures> procedures_;
    std::weak_ptr<ClientRpcContext> context_;
    thallium::engine queryServiceEngine;
    ServiceId queryServiceId;
    std::mutex queryServiceMutex;
    std::atomic<uint32_t> queryIndex;
    int queryTimeoutInSecs;
    std::map<uint32_t, std::shared_ptr<PlaybackQuery>> activeQueryMap; // map of active queries by queryId
    std::map<std::pair<ChronicleName, StoryName>, std::pair<StoryId, PlaybackQueryRpcClient*>> acquiredStoryMap;
    // map of QueryRpcClients by service_endpoint of the remote chrono_grapher PlaybackService
    std::map<service_endpoint, PlaybackQueryRpcClient*> playbackRpcClientMap;
};


} // namespace chronolog

#endif
