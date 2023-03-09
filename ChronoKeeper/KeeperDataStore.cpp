
#include <map>
#include <mutex>

#include "KeeperDataStore.h"

namespace chl = chronolog;

////////////////////////

int chronolog::KeeperDataStore::startStoryRecording(std::string const& chronicle, std::string const& story, chronolog::StoryId const& story_id
		           , uint64_t start_time, uint32_t time_chunk_duration, uint32_t access_window)
{
      std::cout<<"KeeperDataStore: received startStoryRecording {"<< chronicle<<", "<<story<<", "<<story_id<< "}"<<std::endl;

      //get dataStoreMutex, check for story_id_presense & add new StoryPipeline if needed
      std::lock_guard storeLock(dataStoreMutex);
      auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
      if(pipeline_iter == theMapOfStoryPipelines.end())
      {
        auto result = theMapOfStoryPipelines.emplace(std::pair<chl::StoryId, chl::StoryPipeline*> (story_id, new chl::StoryPipeline(chronicle, story, story_id, start_time, time_chunk_duration)));	      
	 if( result.second)
	 { pipeline_iter = result.first; }
	 else
         { return 0; }	 
      }

      //check it the pipeline was put on the waitingForExit list by the previous aquisition
      // and remove it from there
      auto waiting_iter = pipelinesWaitingForExit.find(story_id);
      if(waiting_iter != pipelinesWaitingForExit.end())
      { 
        pipelinesWaitingForExit.erase(waiting_iter);
      }

      //engage StoryPipeline with the IngestionQueue

      StoryIngestionHandle * ingestionHandle = (*pipeline_iter).second->getActiveIngestionHandle();

      theIngestionQueue.addStoryIngestionHandle( story_id, ingestionHandle);

return 1;
}
////////////////////////

int chronolog::KeeperDataStore::stopStoryRecording(chronolog::StoryId const& story_id)
{
      std::cout<<"KeeperDataStore: received stopStoryRecording {"<<story_id<<"}"<< std::endl;

      // we do not yet disengage the StoryPipeline from the IngestionQueue right away
      // but put it on the WaitingForExit list to be finalized, persisted to disk , and 
      // removed from ,memory at time now+acceptance_window...
      // unless there's a new story aqiusition request comes before that moment
      
	std::lock_guard storeLock(dataStoreMutex);
      auto pipeline_iter = theMapOfStoryPipelines.find(story_id);
      if(pipeline_iter != theMapOfStoryPipelines.end())
      {  
         //INNA:set this later (*pipeline_iter).second->scheduleForExit(revisionTime + acceptanceWindow);
	 
	pipelinesWaitingForExit.insert(std::pair<chl::StoryId,chl::StoryPipeline*>( (*pipeline_iter).first, (*pipeline_iter).second) );
      }

return 1;
}
////////////////////////

int  chronolog::KeeperDataStore::readStoryFromArchive(std::string const& archiveLocation
		, std::string const & chronicle, std::string const& story, chronolog::StoryId const & story_id
		, uint64_t start_Time, uint64_t end_time, uint32_t time_chunk_granularity)
{
  //NOT implemented yet
  return 1;
}
////////////////////////

int  chronolog::KeeperDataStore::writeStoryToArchive(std::string const& archiveLocation
		, std::string const & chronicle, std::string const& story, chronolog::StoryId const & story_id
		, uint64_t start_time, uint64_t end_time, uint32_t archive_granularity)
{
  //NOT implemented yet
  return 1;
}
//////////////////////////////

void chronolog::KeeperDataStore::shutdownDataCollection()
{
   if( is_shutting_down() )
   {   return;   }
      
   std::cout<<"KeeperDataStore: shutdownDataCollection" << std::endl;
 // switch the state to shuttingDown
   std::lock_guard storeLock(dataStoreMutex);
   if( is_shutting_down() )
   {   return;   }

   state = SHUTTING_DOWN;

   //stop Recording service for all the active stories
   // ? should we notify the Visor ???
   // 
   // Finalize and persist all the story lines to disk
   // ? we should  probably disregard acceptance window in this case
   //
   // Unregister the ChronoKeeper   
}

///////////////////////

 //
chronolog::KeeperDataStore::~KeeperDataStore()
{
  // in case shutdown was not triggered yet	
  shutdownDataCollection();

}
