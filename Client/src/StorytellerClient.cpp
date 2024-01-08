#include <iostream>

#include <thallium.hpp>
#include <chrono>
#include <climits>

#include "../chrono_common/chronolog_types.h"
#include "StorytellerClient.h"
#include "KeeperRecordingClient.h"

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

//////////////////
template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle <KeeperChoicePolicy>::log_event(std::string const &event_record)
{

    chronolog::LogEvent log_event(storyId, theClient.getTimestamp(), theClient.getClientId()
                                  , theClient.get_event_index(), event_record);

    auto keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
    if(nullptr == keeperRecordingClient)   //very unlikely...
    {
        Logger::getLogger()->warn("[StoryWritingHandle] No keeper selected for logging event: {}", event_record);
        return 0;
    }

    // INNA: make send event returm 0 in case of tl RPC failure ....
    keeperRecordingClient->send_event_msg(log_event);

    return 1;
}
/////////////////////

template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle <KeeperChoicePolicy>::log_event(size_t, void*)
{
    return 0;  // not implemented yet ; to be implemented with tl bulk transfer ... 
}
//////////////////////////////////////////


chronolog::StorytellerClient::~StorytellerClient()
{
    Logger::getLogger()->debug("[StorytellerClient] Destructor called.");
    {
        std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);
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
        std::string a_string;
        std::string keeper_service_na_string =
                rpc_protocol_string + "://" + keeper_id_card.getIPasDottedString(a_string) + ":" +
                std::to_string(keeper_id_card.getPort());
        chronolog::KeeperRecordingClient*keeperRecordingClient = chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(
                client_engine, keeper_id_card, keeper_service_na_string);

        auto insert_return = recordingClientMap.insert(
                std::pair <std::pair <uint32_t, uint16_t>, chronolog::KeeperRecordingClient*>(
                        std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort())
                        , keeperRecordingClient));
        if(false == insert_return.second)
        {
            return 0;
        }
        std::stringstream ss;
        ss << keeper_id_card;
        Logger::getLogger()->debug("[StorytellerClient] Added KeeperRecordingClient for KeeperIdCard: {}", ss.str());
    }
    catch(tl::exception const &ex)
    {
        std::stringstream s1;
        s1 << keeper_id_card;
        Logger::getLogger()->debug("[StorytellerClient] Failed to create KeeperRecordingClient for KeeperIdCard: {}"
                                   , s1.str());
    }

    // state = RUNNING;
    Logger::getLogger()->debug("[StorytellerClient] RUNNING with {} KeeperRecordingClients", recordingClientMap.size());
    return 1;
}
/////////////////

int chronolog::StorytellerClient::removeKeeperRecordingClient(chronolog::KeeperIdCard const &keeper_id_card)
{
    std::lock_guard <std::mutex> lock(recordingClientMapMutex);

    // stop & delete keeperRecordingClient before erasing keeper_process entry
    auto keeper_client_iter = recordingClientMap.find(
            std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
    if(keeper_client_iter != recordingClientMap.end())
    {
        delete (*keeper_client_iter).second;
        recordingClientMap.erase(keeper_client_iter);
    }

    //INNA: TODO: if this function is triggered by the Vizor calls when the ChronoKeeper process unexpectedly unregistered/exited
    // we need to iterate through the known WritingHandles and make sure this keeperClient is removed from all the active storyHandles
    // serialize the log events by switching the state to PENDING and forcing the log event calls to wait by locking
    //recording clientMutex during this time ....
    std::stringstream ss;
    ss << keeper_id_card;
    Logger::getLogger()->debug("[StorytellerClient] Removed KeeperRecordingClient for KeeperIdCard: {}", ss.str());
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
        Logger::getLogger()->info(
                "[StorytellerClient::findStoryWritingHandle] Found StoryHandle for Chronicle: '{}' and Story: '{}'."
                , chronicle, story);
        return ((*story_record_iter).second);
    }
    else
    {
        Logger::getLogger()->warn(
                "[StorytellerClient::findStoryWritingHandle] StoryHandle not found for Chronicle: '{}' and Story: '{}'."
                , chronicle, story);
        return (nullptr);
    }
}

/////////////

chronolog::StoryHandle*
chronolog::StorytellerClient::initializeStoryWritingHandle(ChronicleName const &chronicle, StoryName const &story
                                                           , StoryId const &story_id
                                                           , std::vector <KeeperIdCard> const &vectorOfKeepers)
//INNA: TODO :KeeperChoicePolicy will have to be communicated here as well ....
{
    std::lock_guard <std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair <std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        Logger::getLogger()->info("[StorytellerClient] StoryHandle already exists for Chronicle: '{}' and Story: '{}'."
                                  , chronicle, story);
        return story_record_iter->second;
    }

    // create new StoryWritingHandle & initialize it's keeperClients vector    
    chronolog::StoryWritingHandle <RoundRobinKeeperChoice>*storyWritingHandle = new StoryWritingHandle <RoundRobinKeeperChoice>(
            *this, chronicle, story, story_id);

    for(KeeperIdCard keeper_id_card: vectorOfKeepers)
    {
        auto keeper_client_iter = recordingClientMap.find(
                std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
        if(keeper_client_iter == recordingClientMap.end())
        {
            // unlikely but we better check
            if(addKeeperRecordingClient(keeper_id_card) == 0)
            {
                std::stringstream ss;
                ss << keeper_id_card;
                Logger::getLogger()->warn(
                        "[StorytellerClient] Failed to add KeeperRecordingClient for KeeperIdCard: '{}'.", ss.str());
                continue;
            }
        }
        keeper_client_iter = recordingClientMap.find(
                std::pair <uint32_t, uint16_t>(keeper_id_card.getIPaddr(), keeper_id_card.getPort()));
        storyWritingHandle->addRecordingClient((*keeper_client_iter).second);
    }

    auto insert_return = acquiredStoryHandles.insert(
            std::pair <std::pair <std::string, std::string>, chronolog::StoryHandle*>(
                    std::pair <std::string, std::string>(chronicle, story), storyWritingHandle));
    if(!insert_return.second)
    {
        Logger::getLogger()->error(
                "[StorytellerClient] Failed to insert StoryWritingHandle for Chronicle: '{}' and Story: '{}'."
                , chronicle, story);
        delete storyWritingHandle;
        return nullptr;
    }

    Logger::getLogger()->info(
            "[StorytellerClient] Successfully initialized StoryWritingHandle for Chronicle: '{}' and Story: '{}'."
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
        Logger::getLogger()->info(
                "[StorytellerClient] Successfully removed StoryHandle for Chronicle: '{}' and Story: '{}'.", chronicle
                , story);
    }
    else
    {
        Logger::getLogger()->warn(
                "[StorytellerClient] No matching StoryHandle found for Chronicle: '{}' and Story: '{}'.", chronicle
                , story);
    }
}

/////////////////


