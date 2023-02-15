
#include <map>
#include <mutex>

#include "KeeperDataStore.h"

////////////////////////

chronolog::StoryPipeline::StoryPipeline( chronolog::StoryId const& story_id, std::mutex & ingestion_mutex, std::mutex & sequencing_mutex)
	: storyId(story_id)
	, ingestionMutex(ingestion_mutex)
	, sequencingMutex(sequencing_mutex)  

{

}
///////////////////////

chronolog::StoryPipeline::~StoryPipeline()
{  }

/////////////////////

int chronolog::StoryPipeline::mergeEvents(std::vector<chronolog::LogEvent> const&)
{
  int status =0;

return status;
}
////////////////////

int chronolog::StoryPipeline::mergeEvents(chronolog::StoryChunk const&)
{

  int status =0;







 return status;
}

