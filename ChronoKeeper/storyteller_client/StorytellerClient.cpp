
#include <iostream>

#include <thallium.hpp>
#include <chrono>

#include "chronolog_types.h"
#include "StorytellerClient.h"
#include "KeeperRecordingClient.h"

namespace thallium = tl;

uint64_t chronolog::ChronologTimer::getTimestamp()
{
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}
/////////////////////

chronolog::StorytellerClient::~StorytellerClient()
{
 //not implemented yet
}

int chronolog::StorytellerClient::log_event( chronolog::StoryId  const& story_id, std::string const&)
{
	//reset the index on the chance we've reached the max int value...
       if ( atomic_index= max_index)
       {
	   std::lock_guard<std::mutex> lock(recordingClientMapMutex);
	   if(atomic_index == max_atomic_index)
	   {    atomic_index = 0; }
       }


 return 1;
}

/*int chronolog::StorytellerClient::log_event( chronolog::StoryId const& story_id, size_t , void*)
{
 //not implemented yet	
 return 1;
}
*/

int chronolog::StorytellerClient::addKeeperRecordingClient( chronolog::KeeperIdCard const& keeper_id_card)
{
	   std::lock_guard<std::mutex> lock(recordingClientMapMutex);

           std::string service_na_string("ofi+sockets://");
	   service_na_string = keeper_id_card.getIPasDottedString(service_na_string)
                             + ":"+ std::to_string(keeper_id_card.getPort());
           try
	   {
		chronolog::KeeperRecordingClient * keeperRecordingClient = chronolog::KeeperRecordingClient::CreateKeeperRecordingClient
			 (client_engine, service_na_string, keeper_id_card.getProviderId());
		
		auto insert_return = recordingClientMap.insert( std::pair<std::pair<uint32_t,uint16_t>, chronolog::KeeperRecordingClient*>
		( std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()), keeperRecordingClient)
		);
	   	if( false == insert_return.second)
	   	{
		   return 0;
	   	}

	   	std::cout<<"StorytellerClient: created keeperRecordingClient for {"<<keeper_id_card<<"}"<<std::endl;
	   }
	   catch( tl::exception  const& ex)
	   {
	      std::cout <<"StorytellerClient: failed to create KeeperRecordingClient for {"<<keeper_id_card<<"}"<< std::endl;
	   }

	  // state = RUNNING;
	   std::cout << "StorytellerClient: RUNNING with {" << recordingClientMap.size()<<"} KeeperRecordingClients"<<std::endl;

return 1;
}
/////////////////

int chronolog::StorytellerClient::removeKeeperRecordingClient( chronolog::KeeperIdCard const & keeper_id_card) 
       {
	   std::lock_guard<std::mutex> lock(recordingClientMapMutex);

	   // stop & delete keeperRecordingClient before erasing keeper_process entry
           auto keeper_client_iter = recordingClientMap.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   if(keeper_client_iter != recordingClientMap.end())
	   {
	   // delete keeperCollectionClient before erasing keeper_process entry
	      delete (*keeper_client_iter).second; 
	      recordingClientMap.erase(keeper_client_iter);
	   }
	   // now that we are still holding registryLock
	   // update registryState if needed

	   return 1;
	}
///////////////////////////

KeeperRecordingClient* chronolog::StorytellerClient::chooseKeeperClient( chronolog::StoryId const& storyId)
{
	
	//find the story acqusition record and choose the keeperRecordingClient 
	//out of the available clients 

	auto story_record_found = aquiredStoryRecords.find(storyId); 
	if ( (story_record_found != aquiredStoryRecords.end()) 
	&& !story_record_found.second.storyKeepers.empty() )
	{ 
	    auto const& story_keepers_vector = story_record.second.storyKeepers;
	    return storyKeepers[ atomic_index % storyKeepers.size()];  
	}

 return nullptr;
}
/////////////////

/*
    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for ( KeeperIdCard keeper_id_card : vectorOfKeepers)
    {
        auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
        if(keeper_process_iter == keeperProcessRegistry.end())
        {
	  std::cout<<"WARNING: Registry faield to find Keeper with {"<<keeper_id_card<<"}"<<std::endl;
	  continue;
	}
   	KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	std::cout<<"found keeper_process:"<< &keeper_process<<" "<<keeper_process.idCard <<" "<<keeper_process.adminServiceId<<keeper_process.keeperCollectionClient<<std::endl;;
	if (nullptr == keeper_process.keeperCollectionClient)
	{ 
	  std::cout<<"WARNING: Registry record for{"<<keeper_id_card<<"} is missing keeperCollectionClient"<<std::endl;
	  continue;
	}
	try
	{
	   int rpc_return = keeper_process.keeperCollectionClient->send_start_story_recording(chronicle, story, storyId, story_start_time);
	   if (rpc_return <0)
	   {
	      std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"}"<<std::endl;
	      continue;
	   }
	   std::cout << "Registry notified keeper {"<<keeper_id_card<<"} to start recording story {"<<storyId<<"} start_time {"<<story_start_time<<"}"<<std::endl;
	   keepers_left_to_notify--;
	}
	catch(thallium::exception const& ex)
	{
	  std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"} details: "<<std::endl;
	  continue;
	}
    }

    if ( keepers_left_to_notify == vectorOfKeepers.size())
    {
	  std::cout<<"ERROR: Registry failed to notify the keepers to start recording story {"<<storyId<<"}"<<std::endl;
	  return -1;
    }

   return ret_status;
}*/

