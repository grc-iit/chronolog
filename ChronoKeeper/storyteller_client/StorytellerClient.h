#ifndef STORYTELLER_CLIENT_H
#define STORYTELLER_CLIENT_H


#include <atomic>
#include <map>

#include <thallium.hpp>

#include "KeeperIdCard.h"
#include "chronolog_types.h"

namespace chronolog
{

class ChronologTimer
{
  public:
       uint64_t getTimestamp();
};

class KeeperRecordingClient;


class StorytellerClient
{
public:
    StorytellerClient( ChronologTimer & chronolog_timer, thallium::engine & client_tl_engine, ClientId client_id)
	    : theTimer(chronolog_timer)
	    , client_engine(client_tl_engine)  
	    , clientId(client_id)  
	    , atomic_index{0}  
    {  }

    ~StorytellerClient();

    int addKeeperRecordingClient(KeeperIdCard const&);
    int removeKeeperRecordingClient(KeeperIdCard const&);

    void addAquiredStory(StoryId const&, std::vector<KeeperIdCard> const&);
    void removeAquiredStory(StoryId const&);

    int log_event( StoryId  const& story_id, std::string const&);
    //int log_event( StoryId const& story_id, size_t , void*);

private:
    StorytellerClient(StorytellerClient const&) = delete;
    StorytellerClient & operator= (StorytellerClient const&) = delete;

    thallium::engine & client_engine;
    ChronologTimer & theTimer;
    ClientId	clientId;
    std::atomic<int>  atomic_index;

    std::mutex  recordingClientMapMutex;

    struct AquiredStoryRecord
    {
        ChronicleName  chronicle;
	StoryName      story;
	StoryId        storyId;
	std::vector<KeeperRecordingClient*> storyKeepers;
    };

    std::map<std::pair<uint32_t,uint16_t>, KeeperRecordingClient*> recordingClientMap;
    std::unordered_map<StoryId, AquiredStoryRecord> aquiredStoryRecords;

    uint64_t getTimestamp()
    {   return theTimer.getTimestamp(); }

    KeeperRecordingClient * chooseKeeperRecordingClient( StoryId const&);

};



}//namespace

#endif
