#ifndef PLAYBACK_SERVICE_H
#define PLAYBACK_SERVICE_H

#include <iostream>
#include <mutex>
#include <thallium.hpp>

#include "chronolog_types.h"

#include "ServiceId.h"
#include "ArchiveReadingRequestQueue.h"

namespace tl = thallium;

namespace chronolog
{

class StoryChunkTransferAgent; 

class PlaybackService: public tl::provider <PlaybackService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static PlaybackService*
    CreatePlaybackService(tl::engine &tl_engine, uint16_t service_provider_id, ArchiveReadingRequestQueue & archiveReadingQueue)
    {
        return new PlaybackService(tl_engine, service_provider_id, archiveReadingQueue);
    }

    ~PlaybackService();

    void playback_service_available(tl::request const &request);

    void
    story_playback_request(tl::request const &request, std::string const &chronicle_name, std::string const &story_name
                        , uint64_t start_time, uint64_t end_time, ServiceId const & requesting_service_id);

private:
    PlaybackService(tl::engine &tl_engine, uint16_t service_provider_id
        , ArchiveReadingRequestQueue & reading_queue);

    PlaybackService() = delete;
    PlaybackService(PlaybackService const &) = delete;
    PlaybackService &operator=(PlaybackService const &) = delete;

    tl::engine  playbackEngine;
    ArchiveReadingRequestQueue & theArchiveReadingRequestQueue;
    std::mutex playbackServiceMutex;
    std::map<service_endpoint, StoryChunkTransferAgent*> chunkSenders;
};

}// namespace chronolog

#endif
