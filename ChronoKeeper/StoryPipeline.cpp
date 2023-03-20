
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
	, activeIngestionHandle(nullptr)

{
	activeIngestionHandle = new chl::StoryIngestionHandle( ingestionMutex, &eventQueue1, &eventQueue2);

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
	return activeIngestionHandle;
}

///////////////////////

chronolog::StoryPipeline::~StoryPipeline()
{ // not implemented yet 
	if(activeIngestionHandle == nullptr)
	{    delete activeIngestionHandle;   }
}

/////////////////////

std::map<uint64_t, chronolog::StoryChunk>::iterator chronolog::StoryPipeline::prependStoryChunk()
{
	std::cout << "StoryPipeline: {"<< storyId<<"} prepending at {"<<timelineStart<<"}"<<std::endl;
	auto result= storyTimelineMap.insert( std::pair<uint64_t, chronolog::StoryChunk>
			 ( timelineStart-chunkGranularity, chronolog::StoryChunk(storyId, timelineStart-chunkGranularity, timelineStart)));
	if( !result.second)
	{ 
	    return storyTimelineMap.end(); 
	}	
        else
	{
	    timelineStart -= chunkGranularity;
	    return result.first;
	}
}
/////////////////////////////

std::map<uint64_t, chronolog::StoryChunk>::iterator chronolog::StoryPipeline::appendStoryChunk()
{
  // append the next storyChunk to the storyTimelineMap and return iterator to the new node
	std::cout << "StoryPipeline: {"<< storyId<<"} appending  chunk at {"<<timelineEnd<<"}"<<std::endl;
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

void chronolog::StoryPipeline::collectIngestedEvents()
{
	activeIngestionHandle->swapActiveDeque(); 

	if( !activeIngestionHandle->getPassiveDeque().empty())
	{   mergeEvents(activeIngestionHandle->getPassiveDeque()); }

}
////////////////////

void chronolog::StoryPipeline::mergeEvents(chronolog::EventDeque & event_deque)
{

	if(event_deque.empty())
	{ return;}

	std::lock_guard<std::mutex> lock(sequencingMutex);
	chl::LogEvent event;
    	std::map<uint64_t, chronolog::StoryChunk>::iterator chunk_to_merge_iter;
	while( !event_deque.empty())
	{
		event = event_deque.front();
        	if( timelineStart <= event.time() && event.time() < timelineEnd)
        	{
        		chunk_to_merge_iter = storyTimelineMap.lower_bound(event.time());
        		(*chunk_to_merge_iter).second.insertEvent(event);
   		}
		else if(event.time() >= timelineEnd)
		{  //extend timeline forward
		  	while(event.time() >= timelineEnd)
			{
      				chunk_to_merge_iter= appendStoryChunk();
      				if ( chunk_to_merge_iter == storyTimelineMap.end())
      				{	break; }
			}
			if (chunk_to_merge_iter != storyTimelineMap.end())		
			{	(*chunk_to_merge_iter).second.insertEvent(event);    }
			else
			{	std::cout << "ERROR : StoryPipeline: {"<< storyId<<"} merge discards event  {"<< event.time()<<"}"<<std::endl; }
		}
		else
		{  //extend timeline backward
			while( event.time() < timelineStart)
			{	
	    			chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
	    			if(chunk_to_merge_iter==storyTimelineMap.end())
	    			{ 	break; }
			}
			if (chunk_to_merge_iter != storyTimelineMap.end())		
			{	(*chunk_to_merge_iter).second.insertEvent(event);  }
			else
			{	std::cout << "ERROR : StoryPipeline: {"<< storyId<<"} merge discards event  {"<< event.time()<<"}"<<std::endl; }
		}
		event_deque.pop_front();
	}
}

//////////////////////
// merge the StoryChunk obtained from external source into the StoryPipeline
// note that the granularity of the StoryChunk may be 
// different from that of the StoryPipeline
//
void chronolog::StoryPipeline::mergeEvents(chronolog::StoryChunk & other_chunk)
{

   // we make no assumptions about the startTime or the granularity of the other_chunk 

   if( other_chunk.empty())
   {   return; }

   std::lock_guard<std::mutex> lock(sequencingMutex);

   std::cout << "StoryPipeline: {"<< storyId<<"} merge StoryChunk {"<<other_chunk.getStartTime()<<" : "<<other_chunk.getEndTime()<<"}"<<std::endl;

   // locate the storyChunk in the StoryPipeline with the time Key not less than 
   // other_chunk.startTime and start merging

   std::map<uint64_t, chronolog::StoryChunk>::iterator chunk_to_merge_iter;
   
   if( timelineStart <= other_chunk.getStartTime() )
   {
        chunk_to_merge_iter = storyTimelineMap.lower_bound(other_chunk.getStartTime());
   }
   else 
   {
	// unlikely but possible that we get some delayed events and need to prepend some chunks
	// extending the timeline back to the past
		std::cout << "StoryPipeline: {"<< storyId<<"} merge prepending at {"<<other_chunk.getStartTime()<<"}"<<std::endl;
		while( timelineStart > other_chunk.getStartTime())
		{	
	    	chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
	    	if(chunk_to_merge_iter==storyTimelineMap.end())
	    	{ //INNA:: if prepend fails we have no choice but to discard the events we can't merge !!

	    		std::cout << "ERROR : StoryPipeline: {"<< storyId<<"} merge discards events  {"<<other_chunk.getStartTime()<<"} {"<< timelineStart<<"}"<<std::endl;
	    		other_chunk.eraseEvents(other_chunk.getStartTime(), timelineStart);
	    		chunk_to_merge_iter=storyTimelineMap.begin();
			}	   
		}
   }	   
  
   //iterate through the storyTimelineMap draining the other_chunk events  
   while (chunk_to_merge_iter != storyTimelineMap.end() &&  !other_chunk.empty())
   {
      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);
      chunk_to_merge_iter++;
   }

   // if there are still records in the other_chunk with the timestamps beyond the current timelineEnd 
   // we extend the timeline forward by appending new chunks

   while ( !other_chunk.empty())
   {
      chunk_to_merge_iter= appendStoryChunk();
      if ( chunk_to_merge_iter == storyTimelineMap.end())
      { break; }

      (*chunk_to_merge_iter).second.mergeEvents(other_chunk);

   }	   

   return;
}
