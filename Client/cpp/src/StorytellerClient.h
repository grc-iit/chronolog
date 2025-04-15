#ifndef STORYTELLER_CLIENT_H
#define STORYTELLER_CLIENT_H


#include <atomic>
#include <map>

#include <thallium.hpp>
#include "chrono_monitor.h"
#include "KeeperIdCard.h"
#include "chronolog_types.h"
#include "chronolog_client.h"

#include "ClientQueryService.h"

namespace chronolog
{

class ChronologTimer
{
public:
    uint64_t getTimestamp();
};

class KeeperRecordingClient;
class PlaybackQueryRpcClient;

class RoundRobinKeeperChoice
{
public:
    KeeperRecordingClient*
    chooseKeeper(std::vector <KeeperRecordingClient*> const &vectorOfKeepers, uint64_t chrono_tick)
    {
        return vectorOfKeepers[chrono_tick % vectorOfKeepers.size()];
    }
};


class StorytellerClient
{
public:
    StorytellerClient(ChronologTimer &chronolog_timer, ClientQueryService & clientQueryService
           ,  ClientId const &client_id)
        : theTimer(chronolog_timer)
        , theClientQueryService(clientQueryService)
        , clientId(client_id)
    {
        LOG_DEBUG("[StorytellerClient] Initialized with ClientID: {}", clientId);
    }

    ~StorytellerClient();

    int addKeeperRecordingClient(KeeperIdCard const &);
    int removeKeeperRecordingClient(KeeperIdCard const &);

    StoryHandle*findStoryWritingHandle(ChronicleName const &, StoryName const &);

    StoryHandle*initializeStoryWritingHandle(ChronicleName const &, StoryName const &, StoryId const &
                                             , std::vector <KeeperIdCard> const &, ServiceId const&);

    void removeAcquiredStoryHandle(ChronicleName const &, StoryName const &);

    uint64_t getTimestamp()
    { return theTimer.getTimestamp(); }

    ClientId const &getClientId() const
    { return clientId; }

    int get_event_index();

    ServiceId const& get_local_service_id() const
    { return theClientQueryService.get_service_id(); }

private:
    StorytellerClient(StorytellerClient const &) = delete;

    StorytellerClient &operator=(StorytellerClient const &) = delete;

    ChronologTimer &theTimer;
    ClientQueryService & theClientQueryService;
    ClientId clientId;
    std::atomic <int> atomic_index;

    std::mutex recordingClientMapMutex;
    std::mutex acquiredStoryMapMutex;

    std::map <std::pair <uint32_t, uint16_t>, KeeperRecordingClient*> recordingClientMap;
    std::map <std::pair <std::string, std::string>, StoryHandle*> acquiredStoryHandles;
    std::map <std::pair <uint32_t, uint16_t>, PlaybackQueryRpcClient*> playbackQueryClientMap;

};


// this class definition lives in the client lib
template <class KeeperChoicePolicy>
class StoryWritingHandle: public StoryHandle
{
public:
    StoryWritingHandle(StorytellerClient &client, ChronicleName const &a_chronicle, StoryName const &a_story , StoryId const &story_id)
        : theClient(client)
        , chronicle(a_chronicle), story(a_story), storyId(story_id)
        , keeperChoicePolicy(new KeeperChoicePolicy)
        , playbackQueryClient(nullptr)
    {
        LOG_DEBUG("[StoryWritingHandle] Initialized for Chronicle: {}, Story: {}", a_chronicle, a_story);
    }

    virtual ~StoryWritingHandle();

    virtual int log_event(std::string const &);

   // virtual int log_event(size_t size, void*data);

    virtual int playback_story(uint64_t start, uint64_t end, std::vector<Event> & playback_events);

    void addRecordingClient(KeeperRecordingClient*);
    void removeRecordingClient(KeeperIdCard const &);

    void attachPlaybackQueryClient(PlaybackQueryRpcClient*);
    void detachPlaybackQueryClient();

private:

    StorytellerClient &theClient;
    ChronicleName chronicle;
    StoryName story;
    StoryId storyId;
    KeeperChoicePolicy*keeperChoicePolicy;
    PlaybackQueryRpcClient * playbackQueryClient;
    std::vector <KeeperRecordingClient*> storyKeepers;
    
};

}//namespace

#endif
