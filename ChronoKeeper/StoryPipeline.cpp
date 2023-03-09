
#include <deque>
#include <map>
#include <mutex>


#include "StoryChunk.h"
#include "StoryPipeline.h"
#include "StoryIngestionHandle.h"

namespace chl = chronolog;

////////////////////////

chronolog::StoryPipeline::StoryPipeline( std::string const& chronicle_name, std::string const& story_name,  chronolog::StoryId const& story_id
		, uint64_t start_time, uint64_t chunk_granularity, uint64_t archive_granularity, uint64_t acceptance_window)
	: storyId(story_id)
	, storyState(chronolog::StoryState::PASSIVE)  
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

	 std::cout << "StoryPipeline : {"<<this<<"} created for {"<<chronicleName<<" "<<storyName<<" "<<story_id<<std::endl;

}

chl::StoryIngestionHandle * chl::StoryPipeline::getActiveIngestionHandle()
{
	if(activeQueueHandle == nullptr)
	{
		activeQueueHandle = new chl::StoryIngestionHandle( ingestionMutex, &eventQueue1);
	}

	return activeQueueHandle;
}

///////////////////////

chronolog::StoryPipeline::~StoryPipeline()
{ // not implemented yet 
}

/////////////////////

std::map<uint64_t, chronolog::StoryChunk>::iterator chronolog::StoryPipeline::appendStoryChunk()
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
//////////////////////
// merge the StoryChunkk obtained from external source into the StoryPipeline
// note that the granularity of the StoryChunk may be 
// different from that of the StoryPipeline
//
void chronolog::StoryPipeline::mergeEvents(chronolog::StoryChunk & other_chunk)
{

   // we make no assumptions about the startTime or the granularity of the chunk submitted for merging

   if( other_chunk.empty())
   {   return; }

   if( timelineStart > other_chunk.getEndTime() )
   {
      // all he records in other_chunk have decayed past the timelineStart	   
	std::cout << "StoryPipeline: {"<< storyId<<"} merge ignores StoryChunk {"<<other_chunk.getStartTime()<<" : "<<other_chunk.getEndTime()<<"}"<<std::endl;
	return;
   }
   
     // part of the other_chunk records have decayed past the timelineStart
     // find the first record that is still within story timeline window
     // and use it as the merge_start

      auto merge_start_record_iter = other_chunk.lower_bound(timelineStart);

   // locate the storyChunk in the StoryPipeline with the time Key not less than 
   // other_chunk.startTime and start merging

   auto chunk_to_merge_iter = storyTimelineMap.lower_bound(other_chunk.getStartTime());

   while (chunk_to_merge_iter != storyTimelineMap.end() &&  !other_chunk.empty())
	   // check if the only records left in the other_chunk are those 
	   // that have decayed .... other_chunk.lastRecordTime() < timelineStart
   {
	
      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);

      chunk_to_merge_iter++;
      
   }

   // there might still be records in the story_chunk with the timestamps beyond the current timelineEnd 
   // if that is the case we extend the timeline forward

   while ( !other_chunk.empty())
   {
      chunk_to_merge_iter= appendStoryChunk();
      if ( chunk_to_merge_iter == storyTimelineMap.end())
      { break; }

      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);

   }	   

   return;
}
