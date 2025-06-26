#ifndef STORY_PIPELINE_H
#define STORY_PIPELINE_H

#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <iostream>

#include "chronolog_types.h"
#include "StoryChunk.h"
#include "StoryChunkExtractionQueue.h"

namespace chronolog
{


class StoryChunkIngestionHandle;

class StoryPipeline
{

public:
    StoryPipeline(StoryChunkExtractionQueue &, ChronicleName const &chronicle_name, StoryName const &story_name
                  , StoryId const &story_id, uint64_t start_time, uint16_t chunk_granularity = 60 // seconds
                  , uint16_t acceptance_window = 120 // seconds
    );

    StoryPipeline(StoryPipeline const &) = delete;

    StoryPipeline &operator=(StoryPipeline const &) = delete;

    ~StoryPipeline();


    StoryChunkIngestionHandle*getActiveIngestionHandle();

    void collectIngestedEvents();

    void mergeEvents(StoryChunk &);

    void extractDecayedStoryChunks(uint64_t);

    StoryId const &getStoryId() const
    { return storyId; }

    uint64_t getAcceptanceWindow() const
    { return acceptanceWindow; }

    uint64_t TimelineStart() const
    { return (*storyTimelineMap.begin()).first; }  // storyTimelineMap is never left empty 

    uint64_t TimelineEnd() const
    { return (*storyTimelineMap.rbegin()).second->getEndTime(); } // storyTimelineMap is never left empty

private:

    StoryChunkExtractionQueue &theExtractionQueue;
    StoryId storyId;
    ChronicleName chronicleName;
    StoryName storyName;
    uint64_t chunkGranularity;
    uint64_t acceptanceWindow;
    uint64_t revisionTime; //time of the most recent merge

    // mutex used to protect the IngestionQueue from concurrent access
    // by RecordingService threads
    std::mutex ingestionMutex;
    // two ingestion queues so that they can take turns playing 
    // active/passive ingestion duty
    // 
    std::deque <StoryChunk*> chunkQueue1;
    std::deque <StoryChunk*> chunkQueue2;

    StoryChunkIngestionHandle*activeIngestionHandle;

    // mutex used to protect Story sequencing operations 
    // from concurrent access by the DataStore Sequencing threads
    std::mutex sequencingMutex;

    // map of storyChunks ordered by StoryChunck.startTime
    std::map <chrono_time, StoryChunk*> storyTimelineMap;

    std::map <uint64_t, StoryChunk*>::iterator prependStoryChunk();

    std::map <uint64_t, StoryChunk*>::iterator appendStoryChunk();

    void finalize();
};

}
#endif
