#ifndef PLAYBACK_SERVICE_H
#define PLAYBACK_SERVICE_H

#include <string>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <ServiceId.h>

#include <ArchiveReadingRequestQueue.h>
#include <KeeperHotFetchClient.h>
#include <PlayerDataStore.h>

namespace tl = thallium;

namespace chronolog
{

class QueryResponseAgent;

class PlaybackService: public tl::provider<PlaybackService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackService* CreatePlaybackService(tl::engine& tl_engine,
                                                  uint16_t service_provider_id,
                                                  PlayerDataStore& activeDataStore,
                                                  ArchiveReadingRequestQueue& archiveReadingQueue)
    {
        return new PlaybackService(tl_engine, service_provider_id, activeDataStore, archiveReadingQueue);
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
    PlaybackService(tl::engine& tl_engine,
                    uint16_t service_provider_id,
                    PlayerDataStore& activeDataStore,
                    ArchiveReadingRequestQueue& reading_queue);

    PlaybackService() = delete;
    PlaybackService(PlaybackService const&) = delete;
    PlaybackService& operator=(PlaybackService const&) = delete;

    // Lazily created client of a keeper's story_range_fetch, cached by
    // endpoint. Returns nullptr if the keeper cannot be looked up.
    KeeperHotFetchClient* getHotFetchClient(ServiceId const& keeper_service_id);

    tl::engine playbackEngine;
    chronolog::PlayerDataStore& theActiveDataStore;
    ArchiveReadingRequestQueue& theArchiveReadingRequestQueue;
    std::mutex playbackServiceMutex;
    std::map<service_endpoint, QueryResponseAgent*> responseSenders;
    // guards the hot-fetch client cache only; never held across RPCs
    std::mutex hotFetchMutex;
    std::map<service_endpoint, KeeperHotFetchClient*> hotFetchClients;
};

} // namespace chronolog

#endif
