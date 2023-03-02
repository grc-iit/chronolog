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

public:
      KeeperDataStore()
      {}

      ~KeeperDataStore();

bool is_shutting_down() const
{
	return (false); // INNA: implement state_management
}

int startStoryRecording(ChronicleName const&, StoryName const&, StoryId const&, uint32_t timeGranularity=30 ); //INNA: 30 seconds?
int stopStoryRecording(StoryId const&);

void shutdownDataCollection();

private:
std::unordered_map<StoryId, StoryPipeline> StoryPipelinesMap;
std::mutex theStoryPipelinesMutex;

};

}
#endif
