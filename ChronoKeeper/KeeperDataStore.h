#ifndef KEEPER_DATA_STORE_H
#define KEEPER_DATA_STORE_H

#include <vector>
#include <list>
#include <map>
#include <mutex>

#include "IngestionQueue.h"
#include "StoryPipeline.h"

namespace chronolog
{


class KeeperDataStore
{

enum DataStoreState
{
	UNKNOWN = 0,
	INITIALIZED =1, //  initialized, no active stories
	RUNNING	=2,	//  active stories
	SHUTTING_DOWN=3	// Shutting down services
};


public:
      KeeperDataStore( IngestionQueue & ingestion_queue)
	      : state(UNKNOWN)
	      , theIngestionQueue(ingestion_queue) 
      {}

      KeeperDataStore( KeeperDataStore const&) = delete;
      KeeperDataStore& operator= ( KeeperDataStore const&) = delete;

      ~KeeperDataStore();

      bool is_initialized() const 
      { return (INITIALIZED == state); }

      bool is_running() const 
      { return (RUNNING == state); }

      bool is_shutting_down() const 
      { return (SHUTTING_DOWN == state); }

      int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&
		      , uint64_t start_time =0, uint32_t time_chunk_ranularity=30, uint32_t access_window = 60 ); //INNA: dummy values that eventually would come from Visor Metadata 
      int stopStoryRecording(StoryId const&);

      int  readStoryFromArchive(std::string const& archiveLocation
		, std::string const & chronicle, std::string const& story, chronolog::StoryId const & story_id
		, uint64_t start_time, uint64_t end_time, uint32_t time_chunk_granularity);

      int  writeStoryToArchive(std::string const& archiveLocation
		, std::string const & chronicle, std::string const& story, chronolog::StoryId const & story_id
		, uint64_t start_time, uint64_t end_time, uint32_t archive_granularity);

      void shutdownDataCollection();

private:
      DataStoreState	state;
      IngestionQueue &   theIngestionQueue;
      std::mutex dataStoreMutex;
      std::unordered_map<StoryId, StoryPipeline*> theMapOfStoryPipelines;
      std::unordered_map<StoryId, StoryPipeline*> pipelinesWaitingForExit; //INNA: we might get away with simple list .. revisit later


};

}
#endif
