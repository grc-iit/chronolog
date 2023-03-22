
#include <map>
#include <mutex>

#include "KeeperDataStore.h"

////////////////////////

int chronolog::KeeperDataStore::startStoryRecording(std::string const& chronicle, std::string const& story, chronolog::StoryId const& story_id
		           , uint32_t time_chunk_duration)
{
      std::cout<<"KeeperDataStore: received startStoryRecording {"<< chronicle<<", "<<story<<", "<<story_id<< "}"<<std::endl;
	return 1;
}
////////////////////////

int chronolog::KeeperDataStore::stopStoryRecording(chronolog::StoryId const& story_id)
{
      std::cout<<"KeeperDataStore: received stopStoryRecording {"<<story_id<<"}"<< std::endl;

	return 1;
}
////////////////////////

void chronolog::KeeperDataStore::shutdownDataCollection()
{
      std::cout<<"KeeperDataStore: shutdownDataCollection" << std::endl;
 // switch the state to shuttingDown
}

///////////////////////

 // stop Recording Service
 // finalize & persist all  active stories
 //
chronolog::KeeperDataStore::~KeeperDataStore()
{

}
