#ifndef PLAYBACK_SERVICE_H
#define PLAYBACK_SERVICE_H

#include <functional>
#include <string>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <ServiceId.h>

#include <ArchiveReadingRequestQueue.h>
#include <HotRangeSplit.h>
#include <KeeperHotFetchClient.h>
#include <PlayerDataStore.h>

namespace tl = thallium;

namespace chronolog
{

class QueryResponseAgent;
class PlaybackQueryWork;
struct PlaybackQueryResponse;

class PlaybackService: public tl::provider<PlaybackService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackService* CreatePlaybackService(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                                  uint16_t service_provider_id,
                                                  PlayerDataStore& activeDataStore,
                                                  ArchiveReadingRequestQueue& archiveReadingQueue)
    {
        return CreatePlaybackService(owner, service_provider_id, activeDataStore, archiveReadingQueue, {});
    }

    ~PlaybackService();

    void playback_service_available(tl::request const& request);

    void story_playback_request(tl::request const& request,
                                ServiceId const& requesting_service_id,
                                uint32_t query_id,
                                StoryId const& story_id,
                                ChronicleName const& chronicle_name,
                                StoryName const& story_name,
                                chrono_time const& start_time,
                                chrono_time const& end_time);

private:
    friend struct PlaybackServiceTestAccess;
    friend class PlaybackQueryWork;
    QueryResponseAgent* prepareQuery(ServiceId const&,
                                     uint32_t,
                                     StoryId const&,
                                     ChronicleName const&,
                                     StoryName const&,
                                     chrono_time const&,
                                     chrono_time const&,
                                     rpc::RpcDeadline const&,
                                     std::shared_ptr<PlaybackQueryResponse>&);
    using RegistrationObserver = std::function<void(PlaybackService&, bool)>;
    static PlaybackService* CreatePlaybackService(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                                  uint16_t service_provider_id,
                                                  PlayerDataStore& activeDataStore,
                                                  ArchiveReadingRequestQueue& archiveReadingQueue,
                                                  RegistrationObserver observer)
    {
        if(owner->provider)
            throw tl::exception("Playback service provider already exists");
        auto service = std::shared_ptr<PlaybackService>(new PlaybackService(owner,
                                                                            service_provider_id,
                                                                            activeDataStore,
                                                                            archiveReadingQueue,
                                                                            std::move(observer)));
        // Registration publishes callbacks containing this provider's address.
        // Keep the actual object even if registration or later setup throws.
        owner->provider = service;
        service->registerProcedures();
        return service.get();
    }
    PlaybackService(std::shared_ptr<PlaybackRpcOwner> const& owner,
                    uint16_t service_provider_id,
                    PlayerDataStore& activeDataStore,
                    ArchiveReadingRequestQueue& reading_queue,
                    RegistrationObserver observer);

    void registerProcedures();

    PlaybackService() = delete;
    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;

    // Lazily created client of a keeper's story_range_fetch, cached by
    // endpoint. Returns nullptr if the keeper cannot be looked up.
    KeeperHotFetchClient* getHotFetchClient(ServiceId const& keeper_service_id);

    std::weak_ptr<PlaybackRpcOwner> owner_;
    tl::engine& playbackEngine;
    chronolog::PlayerDataStore& theActiveDataStore;
    ArchiveReadingRequestQueue& theArchiveReadingRequestQueue;
    std::mutex playbackServiceMutex;
    std::map<service_endpoint, QueryResponseAgent*> responseSenders;
    // guards the hot-fetch client cache only; never held across RPCs
    std::mutex hotFetchMutex;
    std::map<service_endpoint, KeeperHotFetchClient*> hotFetchClients;
    // Private fault observer; empty in production. It replaces no registration.
    RegistrationObserver registrationObserver_;
};

} // namespace chronolog

#endif
