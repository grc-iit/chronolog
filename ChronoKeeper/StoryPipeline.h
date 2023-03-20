#ifndef STORY_PIPELINE_H
#define STORY_PIPELINE_H

#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <iostream>

#include "chronolog_types.h"
#include "StoryChunk.h"

namespace chronolog
{


class StoryIngestionHandle;

enum StoryState
{ 
  PASSIVE = 0,
  ACTIVE = 0,
  EXITING = 0
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

     StoryPipeline(StoryPipeline const&) = delete;
     StoryPipeline& operator=(StoryPipeline const&) = delete;

    ~StoryPipeline();


    StoryIngestionHandle * getActiveIngestionHandle();

    void collectIngestedEvents();
    void mergeEvents(std::deque<LogEvent> &);
    void mergeEvents(StoryChunk &);

    void scheduleForExit(uint64_t);

private:

    StoryId 	storyId;
    StoryState  storyState;
    ChronicleName	chronicleName;
    StoryName	storyName;
    uint64_t	timelineStart;
    uint64_t	timelineEnd;
    uint64_t	chunkGranularity;
    uint64_t 	archiveGranularity;
    uint64_t 	acceptanceWindow;
    uint64_t	revisionTime; //time of the most recent merge 
    uint64_t	exitTime; //time the story can be removed from memory

    // mutex used to protect the IngestionQueue from concurrent access
    // by RecordingService threads
    std::mutex ingestionMutex;
    // two ingestion queues so that they can take turns playing 
    // active/passive ingestion duty
    // 
    std::deque<LogEvent> eventQueue1;
    std::deque<LogEvent> eventQueue2;

    StoryIngestionHandle * activeIngestionHandle;

    // mutex used to protect Story sequencing operations 
    // from concurrent access by the DataStore Sequencing threads
    std::mutex sequencingMutex;

    // map of storyChunks ordered by StoryChunck.startTime
    std::map<chrono_time, StoryChunk > storyTimelineMap;

    std::map<uint64_t, StoryChunk>::iterator prependStoryChunk();
    std::map<uint64_t, StoryChunk>::iterator appendStoryChunk();

};

}
#endif
