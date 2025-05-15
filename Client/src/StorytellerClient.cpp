#include <iostream>

#include <thallium.hpp>
#include <chrono>
#include <climits>

#include "../chrono_common/chronolog_types.h"
#include "StorytellerClient.h"
#include "KeeperRecordingClient.h"
#include "PlaybackQueryRpcClient.h"

namespace tl = thallium;

namespace chl = chronolog;

/////////////////////

uint64_t chronolog::ChronologTimer::getTimestamp()
{
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

/////////////////////
chronolog::StoryHandle::~StoryHandle()
{}

////////////////////
template <class KeeperChoicePolicy>
// = chronolog::RoundRobinKeeperChoice>
chronolog::StoryWritingHandle <KeeperChoicePolicy>::~StoryWritingHandle()
{
    delete keeperChoicePolicy;
}

////////////////////
template <class KeeperChoicePolicy>
void
chronolog::StoryWritingHandle <KeeperChoicePolicy>::addRecordingClient(chronolog::KeeperRecordingClient*keeperClient)
{
    storyKeepers.push_back(keeperClient);
}

///////////////////
template <class KeeperChoicePolicy>
void
chronolog::StoryWritingHandle <KeeperChoicePolicy>::removeRecordingClient(chronolog::KeeperIdCard const &keeper_id_card)
{
    // this should only be called when the ChronoKeeper process unexpectedly exits
    // so it's ok to use rather inefficient vector iteration....
    for(auto iter = storyKeepers.begin(); iter != storyKeepers.end(); ++iter)
    {
        if((*iter)->getKeeperId() == keeper_id_card)
        {
            storyKeepers.erase(iter);
            break;
        }
    }
}

template <class KeeperChoicePolicy>
void chronolog::StoryWritingHandle <KeeperChoicePolicy>::attachPlaybackQueryClient(PlaybackQueryRpcClient* playbackClient)
{
    playbackQueryClient = playbackClient;
}

template <class KeeperChoicePolicy>
void chronolog::StoryWritingHandle <KeeperChoicePolicy>::detachPlaybackQueryClient()
{
    playbackQueryClient = nullptr;
}

//////////////////
template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle <KeeperChoicePolicy>::log_event(std::string const &event_record)
{

    chronolog::LogEvent log_event(storyId, theClient.getTimestamp(), theClient.getClientId()
                                  , theClient.get_event_index(), event_record);

    auto keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
    if(nullptr == keeperRecordingClient)   //very unlikely...
    {
        LOG_WARNING("[StoryWritingHandle] No keeper selected for logging event: {}", event_record);
        return 0;
    }

    // INNA: make send event returm 0 in case of tl RPC failure ....
    
    keeperRecordingClient->send_event_msg(log_event);

    //INNA: we probably want to expose the timestamp as the return value here
    // 0 indicates a failure to log as invalid timestamp
    return 1;
}
/////////////////////
/*
template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle <KeeperChoicePolicy>::log_event(size_t, void*)
{
    return 0;  // not implemented yet ; to be implemented with tl bulk transfer ... 
}
*/

template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle<KeeperChoicePolicy>::playback_story(uint64_t start_time, uint64_t end_time
        , std::vector<chronolog::Event>& playback_events)
{
    playback_events.clear();

    if(nullptr == playbackQueryClient)
    { return chronolog::CL_ERR_NO_PLAYERS; }

    playbackQueryClient->send_story_playback_request(chronicle, story, start_time, end_time);

return chronolog::CL_SUCCESS;
}

//////////////////////////////////////////


chronolog::StorytellerClient::~StorytellerClient()
{
    LOG_DEBUG("[StorytellerClient] Destructor called.");
    {
        std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);
        //TODO: INNA: investigate why the folowing lines were commented out in the previous version
        /*
        for( auto story_record_iter : acquiredStoryHandles)
        {
            delete story_record_iter.second;
        }
        acquiredStoryHandles.clear();
  */  }
    // stop & delete keeperRecordingClients
    std::lock_guard <std::mutex> lock(recordingClientMapMutex);
    for(auto keeper_client: recordingClientMap)
    {
        delete keeper_client.second;
    }
    recordingClientMap.clear();
    //delete playback clients
    for(auto playback_client: playbackQueryClientMap)
    {
        delete playback_client.second;
    }
    playbackQueryClientMap.clear();
}

int chronolog::StorytellerClient::get_event_index()
{
    // we only aqcuire mutex in the rare case when
    // the atomic index has reached INT_MAX value
    // otherwise proceed lock-free
    if((atomic_index == INT_MAX))
    {
        std::lock_guard <std::mutex> lock(recordingClientMapMutex);
        //recheck when mutex is acquired, only one thread changes the value
        if(atomic_index == INT_MAX)
        { atomic_index = 0; }
    }
    return ++atomic_index;
}
////////////////


int chronolog::StorytellerClient::addKeeperRecordingClient(chronolog::KeeperIdCard const &keeper_id_card)
{
    std::lock_guard <std::mutex> lock(recordingClientMapMutex);

    try
    {
        chronolog::KeeperRecordingClient*keeperRecordingClient = chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(
                theClientQueryService.get_service_engine(), keeper_id_card);

        auto insert_return = recordingClientMap.insert(
                std::pair <std::pair <uint32_t, uint16_t>, chronolog::KeeperRecordingClient*>(
                        keeper_id_card.getRecordingServiceId().get_service_endpoint(), keeperRecordingClient));
        if(false == insert_return.second)
        {
            LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}", to_string(keeper_id_card));
            return 0;
        }
        LOG_INFO("[StorytellerClient] Added KeeperRecordingClient for {}", to_string(keeper_id_card));
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}", to_string(keeper_id_card));
    }

    // state = RUNNING;
    LOG_INFO("[StorytellerClient] RUNNING with {} KeeperRecordingClients", recordingClientMap.size());
    return 1;
}
/////////////////

int chronolog::StorytellerClient::removeKeeperRecordingClient(chronolog::KeeperIdCard const &keeper_id_card)
{
    std::lock_guard <std::mutex> lock(recordingClientMapMutex);

    // stop & delete keeperRecordingClient before erasing keeper_process entry
    auto keeper_client_iter = recordingClientMap.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
        
    if(keeper_client_iter != recordingClientMap.end())
    {
        delete (*keeper_client_iter).second;
        recordingClientMap.erase(keeper_client_iter);
    }

    //INNA: TODO: if this function is triggered by the Vizor calls when the ChronoKeeper process unexpectedly unregistered/exited
    // we need to iterate through the known WritingHandles and make sure this keeperClient is removed from all the active storyHandles
    // serialize the log events by switching the state to PENDING and forcing the log event calls to wait by locking
    //recording clientMutex during this time ....
    LOG_INFO("[StorytellerClient] Removed KeeperRecordingClient for {}", to_string(keeper_id_card));
    return 1;
}

///////////////////////////
chronolog::StoryHandle*
chronolog::StorytellerClient::findStoryWritingHandle(ChronicleName const &chronicle, StoryName const &story)
{
    std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair <std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_INFO("[StorytellerClient::findStoryWritingHandle] Found StoryHandle for Chronicle: '{}' and Story: '{}'."
             , chronicle, story);
        return ((*story_record_iter).second);
    }
    else
    {
        LOG_WARNING("[StorytellerClient::findStoryWritingHandle] StoryHandle not found for Chronicle: '{}' and Story: '{}'."
             , chronicle, story);
        return (nullptr);
    }
}

/////////////

chronolog::StoryHandle*
chronolog::StorytellerClient::initializeStoryWritingHandle(ChronicleName const &chronicle, StoryName const &story
                                                           , StoryId const &story_id
                                                           , std::vector <KeeperIdCard> const &vectorOfKeepers
                        , chl::ServiceId const & player_card)
//INNA: TODO :KeeperChoicePolicy will have to be communicated here as well ....
{
    std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair <std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_DEBUG("[StorytellerClient] StoryHandle already exists for Chronicle: '{}' and Story: '{}'.", chronicle, story);
        return story_record_iter->second;
    }

    // create new StoryWritingHandle & initialize it's keeperClients vector    
    chronolog::StoryWritingHandle <RoundRobinKeeperChoice>*storyWritingHandle = new StoryWritingHandle <RoundRobinKeeperChoice>(
            *this, chronicle, story, story_id);

    for(KeeperIdCard keeper_id_card: vectorOfKeepers)
    {
        auto keeper_client_iter = recordingClientMap.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());

        if(keeper_client_iter == recordingClientMap.end())
        {
            // unlikely but we better check
            if(addKeeperRecordingClient(keeper_id_card) == 0)
            {
                LOG_WARNING("[StorytellerClient] Failed to add KeeperRecordingClient for {}",  to_string(keeper_id_card));
                continue;
            }
        }
        keeper_client_iter = recordingClientMap.find(keeper_id_card.getRecordingServiceId().get_service_endpoint());
                
        storyWritingHandle->addRecordingClient((*keeper_client_iter).second);
    }

    // find existing or create a new one playbackQueryRpcClient
    if(player_card.is_valid())
    {   
        auto playbackQueryClient = theClientQueryService.addPlaybackQueryClient(player_card);

        if(nullptr != playbackQueryClient)
        {
            storyWritingHandle->attachPlaybackQueryClient(playbackQueryClient);
            LOG_DEBUG("[StorytellerClient] PlaybackQueryClient {}  attached to StoryHandle {} {}", chl::to_string(player_card),chronicle,story);
        }
    }
    else
    {        
        LOG_DEBUG("[StorytellerClient] StoryHandle {} {} doesn't have PlaybackQueryClient", chronicle,story);
    }

    auto insert_return = acquiredStoryHandles.insert(
            std::pair <std::pair <std::string, std::string>, chronolog::StoryHandle*>(
                    std::pair <std::string, std::string>(chronicle, story), storyWritingHandle));
    if(!insert_return.second)
    {
        LOG_ERROR("[StorytellerClient] Failed to insert StoryWritingHandle for Chronicle: '{}' and Story: '{}'.", chronicle
             , story);
        delete storyWritingHandle;
        return nullptr;
    }

    LOG_INFO("[StorytellerClient] Successfully initialized StoryWritingHandle for Chronicle: '{}' and Story: '{}'."
         , chronicle, story);
    return storyWritingHandle;
    /*
    // now check the state of the handle:
    // it's possible the other thread is still pending the acquisition response from the Vizor,
    // or the handle's keeper vector is being updated , etc ....
    if (state == PENDING_RESPONSE || state== UPDATING_KEEPERS) )
    {
    // get the handle lock and wait for the thread that sent the request to Vizor to get the response
        std::lock_guard<std::mutex> story_lock(storyHandleMutex);

    }
    */
}

//////////////////////
void chronolog::StorytellerClient::removeAcquiredStoryHandle(ChronicleName const &chronicle, StoryName const &story)
{
    std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair <std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        delete (*story_record_iter).second;
        acquiredStoryHandles.erase(story_record_iter);
        LOG_INFO("[StorytellerClient] Successfully removed StoryHandle for Chronicle: '{}' and Story: '{}'.", chronicle
             , story);
    }
    else
    {
        LOG_WARNING("[StorytellerClient] No matching StoryHandle found for Chronicle: '{}' and Story: '{}'.", chronicle
             , story);
    }
}

/////////////////


