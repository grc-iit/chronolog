#ifndef PLAYBACK_SERVICE_H
#define PLAYBACK_SERVICE_H

#include <iostream>
#include <thallium.hpp>

#include "chronolog_types.h"

#include "ServiceId.h"
#include "ArchiveReadingAgent.h"
#include "PlayerDataStore.h"

namespace tl = thallium;

namespace chronolog
{

class PlaybackService: public tl::provider <PlaybackService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackService*
    CreatePlaybackService(tl::engine &tl_engine, uint16_t service_provider_id, ArchiveReadingAgent & archiveReadingAgentInstance, PlayerDataStore &dataStoreInstance)
    {
        return new PlaybackService(tl_engine, service_provider_id, archiveReadingAgentInstance, dataStoreInstance);
    }

    ~PlaybackService();

    void playback_service_available(tl::request const &request);

    void
    on_story_playback_request(tl::request const &request, std::string const &chronicle_name, std::string const &story_name
                        , uint64_t start_time, uint64_t end_time, ServiceId const & requesting_service_id);

private:
    PlaybackService(tl::engine &tl_engine, uint16_t service_provider_id
        , ArchiveReadingAgent & reading_Agent_instance, PlayerDataStore &data_store_instance);

    PlaybackService(PlaybackService const &) = delete;

    PlaybackService &operator=(PlaybackService const &) = delete;

    ArchiveReadingAgent & theArchiveReadingAgent;
    PlayerDataStore & theDataStore;
};

}// namespace chronolog

#endif
