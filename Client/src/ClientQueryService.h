#ifndef CLIENT_QUERY_SERVICE_H
#define CLIENT_QUERY_SERVICE_H

#include <atomic>
#include <map>
#include <mutex>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "ServiceId.h"

#include "chronolog_client.h"

namespace tl = thallium;

namespace chronolog
{

class PlaybackQueryRpcClient;
class StoryChunk;

struct PlaybackQuery
{
    std::vector<Event>  & eventSeries; 
    uint32_t queryId;
    uint64_t timeout_time;
    bool completed;
    ChronicleName chronicleName;
    StoryName   storyName;
    chrono_time startTime;
    chrono_time endTime;
    std::map<uint64_t,StoryChunk*> PlaybackResponse;

    PlaybackQuery( std::vector<Event> & playbackEvents, uint32_t query_id, uint64_t timeout_time,
            ChronicleName const& chronicle, StoryName const& story, chrono_time const& start, chrono_time const& end)
    : eventSeries(playbackEvents), queryId(query_id),timeout_time(timeout_time), completed(false)
    , chronicleName(chronicle), storyName(story), startTime(start),endTime(end)
    { }
};

class ClientQueryService : public tl::provider <ClientQueryService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static ClientQueryService *
    CreateClientQueryService(thallium::engine & tl_engine, ServiceId const& client_service_id)
    {
        try 
        {
            return new ClientQueryService(tl_engine, client_service_id);
        }
        catch(thallium::exception &)
        {
            return nullptr;
        }
    }

    ~ClientQueryService();

    tl::engine & get_service_engine()
    { return queryServiceEngine; }

    ServiceId const& get_service_id() const
    { return queryServiceId; }

    int addStoryReader(ChronicleName const&, StoryName const&, ServiceId const&);
    void removeStoryReader(ChronicleName const&, StoryName const&);
 
    void receive_story_chunk(tl::request const&, tl::bulk &);

    int replay_story( ChronicleName const&, StoryName const&, uint64_t start, uint64_t end, std::vector<Event> & replay_events);

private:
    ClientQueryService(thallium::engine & tl_engine, ServiceId const&);

    ClientQueryService() = delete;
    ClientQueryService(ClientQueryService const&) = delete;

    // create PlaybackServiceRpcClient associated with the remote Playback Service
    PlaybackQueryRpcClient * addPlaybackQueryClient(ServiceId const& );
    // destroy PlaybackServiceRpcClient associated with the remote Playback Service
    void removePlaybackQueryClient(ServiceId const& );

    PlaybackQuery * start_query(uint64_t, ChronicleName const&, StoryName const&, chrono_time const&, chrono_time const&, std::vector<Event>&);
    void stop_query(uint32_t);

    int deserializedWithCereal(char *buffer, size_t size, StoryChunk &story_chunk);
    thallium::engine  queryServiceEngine;
    ServiceId       queryServiceId;
    std::mutex queryServiceMutex;    
    std::atomic<int> queryIdIndex;
    std::map<uint32_t, PlaybackQuery> activeQueryMap; // map of active queries by queryId
    std::map<std::pair<ChronicleName,StoryName>, PlaybackQueryRpcClient*> acquiredStoryMap;
    // map of QueryRpcClients by service_endpoint of the remote chrono_grapher PlaybackService
    std::map<service_endpoint, PlaybackQueryRpcClient*> playbackRpcClientMap; 
};


}

#endif
