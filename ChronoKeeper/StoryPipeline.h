#ifndef STORY_PIPELINE_H
#define STORY_PIPELINE_H

#include <deque>
#include <list>
#include <map>
#include <mutex>

#include "chronolog_types.h"

namespace chronolog
{


typedef uint64_t StoryId;
typedef uint64_t StorytellerId;
typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

class StoryIngestionHandle;

// StoryChunk contains all the events for the single story
// for the duration [startTime, endTime[
// startTime included, endTime excluded
// startTime/endTime are invariant
class StoryChunk
{
public:
   
   StoryChunk( StoryId const & story_id = 0, uint64_t start_time = 0, uint64_t end_time = 0)
	   : storyId(story_id)
	   , startTime(start_time)
	   , endTime(end_time)
	   , revisionTime(start_time)
	{ }

   ~StoryChunk() = default;

   uint64_t getStartTime() const
   { return startTime; }
   uint64_t getEndTime() const
   { return endTime; }

   bool empty() const
   {  return ( logEvents.empty() ? true : false ); }

   int mergeEvents(LogEvent const& event)
   {
      if((event.time() >= startTime)
	      && (event.time() < endTime))
      { 
          logEvents.insert( std::pair<ChronoTick,LogEvent>(event.chronoTick, event));
	  return 1;
      }
      else
      {   return 0;   }
   }


   int mergeEvents(StoryChunk const&)
   {  return 1; }

private:
StoryId storyId;          
uint64_t startTime;
uint64_t endTime;   
uint64_t revisionTime;
std::map<ChronoTick, LogEvent> logEvents;

};

class StoryPipeline
{
public:
    StoryPipeline( std::string const& chronicle_name, std::string const& story_name
			, StoryId const& story_id
			, uint64_t start_time
		        , uint64_t chunk_granularity = 300 //5 mins 
			, uint64_t archive_granularity = 24* 20 * 300 // 24 hours
			, uint64_t acceptance_window = 2 * 300 // 10 mins
		    
		     );
	
    ~StoryPipeline();

   
    int mergeEvents(std::deque<LogEvent> const&);
    int mergeEvents(StoryChunk const&);

private:

    StoryId 	storyId;
    ChronicleName	chronicleName;
    StoryName	storyName;
    uint64_t	timelineStart;
    uint64_t	timelineEnd;
    uint64_t	chunkGranularity;
    uint64_t 	archiveGranularity;
    uint64_t 	acceptanceWindow;
    uint64_t	revisionTime; //timestamp of the most recent merge 
    uint64_t	tailTime; // timestamp of the most recent recorded event
    uint64_t	acquisitionTime; //timestamp of the most recent aquisition

    // mutex used to protect the IngestionQueue from concurrent access
    // by RecordingService threads
    std::mutex ingestionMutex;
    // two ingestion queues so that they can take turns playing 
    // active/passive ingestion duty
    // 
    std::deque<LogEvent> eventQueue1;
    std::deque<LogEvent> eventQueue2;

    StoryIngestionHandle * activeQueueHandle;

    // mutex used to protect Story sequencing operations 
    // from concurrent access by the DataStore Sequencing threads
    std::mutex sequencingMutex;

    // map of storyChunks ordered by StoryChunck.startTime
    std::map<chrono_time, StoryChunk > storyTimelineMap;

    std::map<uint64_t, StoryChunk>::iterator appendNextStoryChunk();

};

}
#endif
