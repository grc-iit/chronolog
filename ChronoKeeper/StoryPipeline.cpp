
#include <deque>
#include <map>
#include <mutex>

#include "StoryPipeline.h"

////////////////////////

chronolog::StoryPipeline::StoryPipeline( std::string const& chronicle_name, std::string const& story_name,  chronolog::StoryId const& story_id
		, uint64_t start_time, uint64_t chunk_granularity, uint64_t archive_granularity, uint64_t acceptance_window)
	: storyId(story_id)
	, chronicleName(chronicle_name)
	, storyName(story_name)
	, timelineStart(start_time -acceptance_window)   
	, timelineEnd(start_time)
	, chunkGranularity(chunk_granularity)
	, archiveGranularity(archive_granularity)  
	, acceptanceWindow(acceptance_window)  
	, activeQueueHandle(nullptr)

{
	//pre-initialize the pipeline map with the StoryChunks of timelineGranulary 
	// with the total time window of 1/2 archiveGranularity
	// current defaults are 5 mins chunks for 12 hours total
         
	 timelineStart -= (timelineStart%chunkGranularity); //make sure timelineStart is on the boundary of chunkGranularity
							    //
	 for(uint64_t start = timelineStart; timelineEnd < (timelineStart + archiveGranularity/2 );)
	 {
		auto result= storyTimelineMap.insert( std::pair<uint64_t, chronolog::StoryChunk>( start, StoryChunk(storyId, start, start+chunkGranularity)) );
		if( !result.second)
		{   break;   }

		timelineEnd = start+chunkGranularity;
		start = timelineEnd;

	 }

}
///////////////////////

chronolog::StoryPipeline::~StoryPipeline()
{  }

/////////////////////

std::map<uint64_t, chronolog::StoryChunk>::iterator chronolog::StoryPipeline::appendNextStoryChunk()
{
  // append the next storyChunk to the storyTimelineMap and return iterator to the new node
	auto result= storyTimelineMap.insert( std::pair<uint64_t, chronolog::StoryChunk>( timelineEnd, chronolog::StoryChunk(storyId, timelineEnd, timelineEnd+chunkGranularity)));
	if( !result.second)
	{ 
	    return storyTimelineMap.end(); 
	}	
        else
	{
	    timelineEnd += chunkGranularity;
	    return result.first;
	}
}
int chronolog::StoryPipeline::mergeEvents(chronolog::StoryChunk const& other_chunk)
{

  int status =0;

  //INNA: TODO: check the case when storyChunk.startTime < timelineStart....
 
   // we make no assumptions about the granularity of the chunk submitted for merging
   // so first locate the storyChunk in the StoryPipeline with the time Key not less than 
   // other_chunk.startTime

   auto chunk_to_merge_iter = storyTimelineMap.lower_bound(other_chunk.getStartTime());

   while (chunk_to_merge_iter != storyTimelineMap.end() &&  !other_chunk.empty())
   {
	
      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);
      chunk_to_merge_iter++;
      
   }

   if (other_chunk.empty())
	   return 1;

   // there are records in the other_chunk with the timestamps beyond the last timeChunk in the StoryTimelineMap
   // extend the timeline forward

   while ( !other_chunk.empty())
   {
      chunk_to_merge_iter= appendNextStoryChunk();
      if ( chunk_to_merge_iter == storyTimelineMap.end())
      { break; }

      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);

   }	   

 return (other_chunk.empty() ? ! 1 : 0);
}
