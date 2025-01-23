#ifndef CLIENT_QUERY_SERVICE_H
#define CLIENT_QUERY_SERVICE_H

#include <atomic>
#include <map>
#include <mutex>
#include <thallium.hpp>

#include "chronolog_types.h"


namespace chronolog
{

class PlaybackQueryRpcClient;
class StoryChunk;

struct StoryPlaybackQuery
{
    uint32_t queryId;
    ChronicleName chronicleName;
    StoryName   storyName;
    chrono_time startTime;
    chrono_time endTime;
    StoryId     storyId;
    PlaybackQueryRpcClient * playbackRpcClient;
    std::map<uint64_t,StoryChunk*> PlaybackResponse;
};

class ClientQueryService
{
public:
    static ClientQueryService *
    CreateClientQueryService(thallium::engine & tl_engine, ServiceId const& client_query_service_id);

    ~ClientQueryService();

    // send the playback request to the ChronoPlayer PlaybackService and return queryId
    uint32_t send_playback_query_request(ServiceId const& playback_service_id,
                ChronicleName const&, StoryName const&,chrono_time const&, chrono_time const&);    

    int receive_story_chunk();

private:
    ClientQueryService(thallium::engine & tl_engine, ServiceId const&);

    ClientQueryService() = delete;
    ClientQueryService(ClientQueryService const&) = delete;

    thallium::engine & clientEngine;
    std::mutex queryServiceMutex;    
    std::atomic<int> queryIdIndex;
    std::map<uint32_t, StoryPlaybackQuery> activeQueryMap; // map of active queries by queryId
};


}

#endif
