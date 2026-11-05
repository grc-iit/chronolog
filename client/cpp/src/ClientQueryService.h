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

struct ReplayEntry
{
    ClientRpcContext::NativeLease native;
    RpcPolicyContext::Snapshot policy;
    ReplayEntry(ClientRpcContext::NativeLease lease, RpcPolicyContext::Snapshot snapshot)
        : native(std::move(lease))
        , policy(std::move(snapshot))
    {}
};
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
    // Entry leases and the context's logical query permit survive every owner,
    // including quarantined outbound work and an active receive callback.
    std::shared_ptr<void> retention;
    std::atomic<bool> submissionReady{false};
    int submissionStatus = CL_ERR_UNKNOWN;
    bool submissionKnown = false;
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
    // B12: prune inert map roots before testing NativeLease/query idleness.
    // False means the lock/map is not drained; unknown work stays charged.
    bool pruneConsumedQueries() noexcept;

    int replay_story(ChronicleName const&,
                     StoryName const&,
                     uint64_t,
                     uint64_t,
                     std::vector<Event>&,
                     rpc::RpcDeadline const&,
                     std::shared_ptr<ReplayEntry>);
    int replay_story(ChronicleName const&,
                     StoryName const&,
                     uint64_t,
                     uint64_t,
                     Client::EventCallback,
                     rpc::RpcDeadline const&,
                     std::shared_ptr<ReplayEntry>);

private:
    friend struct ClientQueryServiceTestAccess;
    ClientQueryService(thallium::engine& tl_engine,
                       ServiceId const&,
                       std::shared_ptr<ClientRpcProcedures>,
                       std::weak_ptr<ClientRpcContext>);

    ClientQueryService() = delete;
    ClientQueryService(ClientQueryService const&) = delete;

    std::shared_ptr<PlaybackQueryRpcClient> addPlaybackQueryClient(ServiceId const&);
    void stop_query(uint32_t);
    int replay(ChronicleName const&,
               StoryName const&,
               uint64_t,
               uint64_t,
               std::vector<Event>*,
               Client::EventCallback,
               rpc::RpcDeadline const&,
               std::shared_ptr<ReplayEntry>);
    int dispatch_query(std::shared_ptr<PlaybackQuery> const&,
                       std::shared_ptr<PlaybackQueryRpcClient> const&,
                       std::shared_ptr<ClientRpcContext> const&,
                       rpc::RpcDeadline const&,
                       uint64_t);
    void registerProcedures(std::function<void(ClientQueryService&, bool)> const& observer);
    std::shared_ptr<ClientRpcProcedures> procedures_;
    std::weak_ptr<ClientRpcContext> context_;
    thallium::engine queryServiceEngine;
    ServiceId queryServiceId;
    std::timed_mutex queryServiceMutex;
    // The unchanged response wire has no native-context generation. Preserve
    // IDs across context retirement/reconnect while this library stays loaded.
    inline static std::atomic<uint64_t> queryIndex{0};
    std::map<uint32_t, std::shared_ptr<PlaybackQuery>> activeQueryMap; // map of active queries by queryId
    std::map<std::pair<ChronicleName, StoryName>, std::pair<StoryId, std::shared_ptr<PlaybackQueryRpcClient>>>
            acquiredStoryMap;
    // map of QueryRpcClients by service_endpoint of the remote chrono_grapher PlaybackService
    std::map<service_endpoint, std::shared_ptr<PlaybackQueryRpcClient>> playbackRpcClientMap;
};


} // namespace chronolog

#endif
