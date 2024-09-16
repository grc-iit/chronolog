#include <deque>
#include <map>
#include <mutex>


#include "StoryChunk.h"
#include "StoryPipeline.h"
#include "StoryChunkIngestionHandle.h"
#include "StoryChunkExtractionQueue.h"
#include "chrono_monitor.h"

//#define TRACE_CHUNKING
#define TRACE_CHUNK_EXTRACTION

namespace chl = chronolog;

////////////////////////

chronolog::StoryPipeline::StoryPipeline(StoryChunkExtractionQueue &extractionQueue, chronolog::ChronicleName const &chronicle_name
                                        , chronolog::StoryName const &story_name, chronolog::StoryId const &story_id
                                        , uint64_t story_start_time, uint16_t chunk_granularity
                                        , uint16_t acceptance_window): theExtractionQueue(extractionQueue), storyId(
        story_id), chronicleName(chronicle_name), storyName(story_name), timelineStart(story_start_time), timelineEnd(
        story_start_time), chunkGranularity(chunk_granularity), acceptanceWindow(acceptance_window)
                                                                       , activeIngestionHandle(nullptr)
{
    activeIngestionHandle = new chl::StoryChunkIngestionHandle(ingestionMutex, &chunkQueue1, &chunkQueue2);

    //pre-initialize the pipeline map with the StoryChunks of chunkGranulary 
    // with the total timelength of at least 2 chunks (merging logic assumes at least 2 chunks in the active pipeline)

    auto story_start_point = std::chrono::time_point <std::chrono::system_clock, std::chrono::nanoseconds>{} +
                             std::chrono::nanoseconds(timelineStart);
    std::time_t time_t_story_start = std::chrono::high_resolution_clock::to_time_t(story_start_point);
    LOG_INFO("[StoryPipeline] Initialized : Chronicle {} Story {} StoryId {} starting at {} "
         , chronicleName, storyName, storyId, std::ctime(&time_t_story_start));

    chunkGranularity *= 1000000000;    // seconds =>nanoseconds
    acceptanceWindow *= 1000000000;    // seconds =>nanoseconds

    //adjust the timelineStart to the closest prior boundary of chunkGranularity
    timelineStart -= (timelineStart % chunkGranularity);
    timelineEnd = timelineStart;

    while( timelineEnd < (timelineStart + chunkGranularity * 3))
    {
        appendStoryChunk();
    }

    LOG_DEBUG("[StoryPipeline] Initialized pipeline : Chronicle {} Story {} StoryId {} timeline {}-{} Granularity {} AcceptanceWindow {}"
         , chronicleName, storyName, storyId, timelineStart, timelineEnd, chunkGranularity, acceptanceWindow );

#ifdef TRACE_CHUNKING
    auto chunk_start_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{} // epoch_time_point{};
        + std::chrono::nanoseconds(timelineStart);
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{}
        + std::chrono::nanoseconds(timelineEnd);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point); 
    LOG_TRACE("[StoryPipeline] Created StoryPipeline with StoryId {}, StartChunk {}, EndChunk {}",
                               storyId, std::ctime(&time_t_chunk_start), std::ctime(&time_t_chunk_end));
#endif

}
///////////////////////

chl::StoryChunkIngestionHandle*chl::StoryPipeline::getActiveIngestionHandle()
{
    return activeIngestionHandle;
}

///////////////////////
chronolog::StoryPipeline::~StoryPipeline()
{
    LOG_DEBUG("[StoryPipeline] Destructor called for StoryId {}", storyId);
    finalize();
}
///////////////////////

void chronolog::StoryPipeline::finalize()
{
    //by this time activeIngestionHandle is disengaged from the IngestionQueue 
    // as part of KeeperDataStore::shutdown
    if(activeIngestionHandle != nullptr)
    {
        while(!activeIngestionHandle->getPassiveDeque().empty())
        {
            StoryChunk* next_chunk = activeIngestionHandle->getPassiveDeque().front();
            activeIngestionHandle->getPassiveDeque().pop_front();
            mergeEvents(*next_chunk);
            delete next_chunk; 
        }
        while(!activeIngestionHandle->getActiveDeque().empty())
        {
            StoryChunk* next_chunk = activeIngestionHandle->getActiveDeque().front();
            activeIngestionHandle->getActiveDeque().pop_front();
            mergeEvents(*next_chunk);
            delete next_chunk; 
        }
        delete activeIngestionHandle;
        LOG_INFO("[StoryPipeline] Finalized ingestion handle for storyId {}", storyId);
    }

    //extract any remianing non-empty StoryChunks regardless of decay_time
    // an active pipeline is guaranteed to have at least 2 chunks at any moment...
    {
        std::lock_guard <std::mutex> lock(sequencingMutex);
        while(!storyTimelineMap.empty())
        {
            StoryChunk*extractedChunk = nullptr;

            extractedChunk = (*storyTimelineMap.begin()).second;
            storyTimelineMap.erase(storyTimelineMap.begin());

            LOG_DEBUG("[StoryPipeline] Finalized chunk for StoryId {} startTime {} eventCount {}", storyId, 
                    extractedChunk->getStartTime(), extractedChunk->getEventCount());

            if(extractedChunk->empty())
            {  // no need to carry an empty chunk any further...
                delete extractedChunk;
            }
            else
            {
                theExtractionQueue.stashStoryChunk(extractedChunk);
            }
        }
    }
}


/////////////////////

std::map <uint64_t, chronolog::StoryChunk*>::iterator chronolog::StoryPipeline::prependStoryChunk()
{
    // prepend a storyChunk at the begining of  storyTimeline and return the iterator to the new node
#ifdef TRACE_CHUNKING
    std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds> epoch_time_point{};
    auto chunk_start_point = epoch_time_point + std::chrono::nanoseconds(timelineStart);
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = epoch_time_point + std::chrono::nanoseconds(timelineStart-chunkGranularity);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point);
    LOG_TRACE("[StoryPipeline] Prepending new chunk for StoryId {} starting at {}",
                               storyId, timelineStart);
#endif
    auto result = storyTimelineMap.insert(
            std::pair <uint64_t, chronolog::StoryChunk*>(timelineStart - chunkGranularity, new chronolog::StoryChunk(
                    chronicleName, storyName, storyId, timelineStart - chunkGranularity, timelineStart)));
    if(!result.second)
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

std::map <uint64_t, chronolog::StoryChunk*>::iterator chronolog::StoryPipeline::appendStoryChunk()
{
    // append the next storyChunk at the end of storyTimeline and return the iterator to the new node
#ifdef TRACE_CHUNKING
    std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds> epoch_time_point{};
    auto chunk_start_point = epoch_time_point + std::chrono::nanoseconds(timelineEnd);
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = epoch_time_point + std::chrono::nanoseconds(timelineEnd+chunkGranularity);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point);
    LOG_TRACE("[StoryPipeline] Appending new chunk for StoryId {} starting at {}",
                               storyId, timelineEnd);
#endif
    auto result = storyTimelineMap.insert(
            std::pair <uint64_t, chronolog::StoryChunk*>(timelineEnd, new chronolog::StoryChunk(chronicleName, storyName, storyId, timelineEnd,timelineEnd + chunkGranularity)));
    if(!result.second)
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
    StoryChunk * next_chunk = nullptr;
    while(!activeIngestionHandle->getPassiveDeque().empty())
    {
        next_chunk = activeIngestionHandle->getPassiveDeque().front();
        activeIngestionHandle->getPassiveDeque().pop_front();
        if( next_chunk != nullptr)
        {
            mergeEvents(*next_chunk);
            delete next_chunk;
        }
    }
}

void chronolog::StoryPipeline::extractDecayedStoryChunks(uint64_t current_time)
{
#ifdef TRACE_CHUNK_EXTRACTION
    auto current_point =
            std::chrono::time_point <std::chrono::system_clock, std::chrono::nanoseconds>{} // epoch_time_point{};
            + std::chrono::nanoseconds(current_time);
    std::time_t time_t_current_time = std::chrono::high_resolution_clock::to_time_t(current_point);
    uint64_t head_chunk_end_time = (*storyTimelineMap.begin()).second->getEndTime();
    auto decay_point =
            std::chrono::time_point <std::chrono::system_clock, std::chrono::nanoseconds>{} // epoch_time_point{};
            + std::chrono::nanoseconds(head_chunk_end_time + acceptanceWindow);
    std::time_t time_t_decay = std::chrono::high_resolution_clock::to_time_t(decay_point);
    LOG_TRACE("[StoryPipeline] StoryId: {} - Current time: {} - Timeline size: {} - Head chunk decay time: {}", storyId
         , std::ctime(&time_t_current_time), storyTimelineMap.size(), std::ctime(&time_t_decay));
#endif

    while(current_time >= acceptanceWindow + (*storyTimelineMap.begin()).second->getEndTime())
    {
        StoryChunk*extractedChunk = nullptr;

        {
            // lock the TimelineMap & check that the decayed storychunk is still there
            std::lock_guard <std::mutex> lock(sequencingMutex);
            if(current_time > acceptanceWindow + (*storyTimelineMap.begin()).second->getEndTime())
            {
                extractedChunk = (*storyTimelineMap.begin()).second;
                storyTimelineMap.erase(storyTimelineMap.begin());
                if(storyTimelineMap.size() < 2)
                    //keep at least 2 chunks in the map of active pipeline as merging relies on it ...
                { appendStoryChunk(); }
            }
        }

        if(extractedChunk != nullptr)
        {
            LOG_TRACE("[StoryPipeline] StoryId: {} - Extracted chunk {}-{} eventCount {}", storyId
                 , extractedChunk->getStartTime(), extractedChunk->getEndTime(), extractedChunk->getEventCount());

            if(extractedChunk->empty())
            {   // there's no need to carry an empty chunk any further...  
                delete extractedChunk;
            }
            else
            {
                theExtractionQueue.stashStoryChunk(extractedChunk);
            }
        }
    }

    LOG_TRACE("[StoryPipeline] Extracting decayed chunks for StoryId {} ExtractionQueue size {}", storyId
         , theExtractionQueue.size());

}

//////////////////////
// Merge the StoryChunk obtained from external source into the StoryPipeline
// Note that the granularity of the StoryChunk being merged may be 
// different from that of the StoryPipeline
//
void chronolog::StoryPipeline::mergeEvents(chronolog::StoryChunk &other_chunk)
{
    // we make no assumptions about the startTime or the granularity of the other_chunk

    if(other_chunk.empty())
    { return; }

    std::lock_guard <std::mutex> lock(sequencingMutex);

    LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {}-{} eventCount {}", storyId,timelineStart, timelineEnd,  other_chunk.getStartTime()
         , other_chunk.getEndTime(), other_chunk.getEventCount());

    // locate the storyChunk in the StoryPipeline with the time Key not less than
    // other_chunk.startTime and start merging
    std::map <uint64_t, chronolog::StoryChunk*>::iterator chunk_to_merge_iter;

    if(timelineStart <= other_chunk.getStartTime())
    {
        // find the chunk_to_merge into : we are lookingt for
        // the chunk preceeding the one with the startTime > other_chunk.getStartTime()
        chunk_to_merge_iter = --storyTimelineMap.upper_bound(other_chunk.getStartTime());
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {} starts with chunk {}", storyId, timelineStart, timelineEnd, 
                other_chunk.getStartTime(), (*chunk_to_merge_iter).second->getStartTime());
    }
    else
    {
        // unlikely but possible that we get some delayed events and need to prepend some chunks
        // extending the timeline back to the past
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {} -timelineStart {} prepending chunks ", storyId, timelineStart, timelineEnd, other_chunk.getStartTime(), timelineStart);

        while(timelineStart > other_chunk.getStartTime())
        {
            chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
            if(chunk_to_merge_iter == storyTimelineMap.end())
            {
                // if prepend fails we have no choice but to discard the events we can't merge !!
                LOG_ERROR("[StoryPipeline] StoryId {} timeline {}-{} : Merge operation discards events between timestamps: {} and {}"
                     , storyId, timelineStart, timelineEnd, other_chunk.getStartTime(), timelineStart);
                other_chunk.eraseEvents(other_chunk.getStartTime(), timelineStart);
                chunk_to_merge_iter = storyTimelineMap.begin();
            }
        }
    }

    // iterate through the storyTimelineMap draining the other_chunk events
    while(chunk_to_merge_iter != storyTimelineMap.end() && !other_chunk.empty())
    {
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {} into chunk {}", storyId, timelineStart, timelineEnd, 
                other_chunk.getStartTime(), (*chunk_to_merge_iter).second->getStartTime());
        (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
        chunk_to_merge_iter++;
    }

    // if there are still records in the other_chunk with the timestamps beyond the current timelineEnd
    // we extend the timeline forward by appending new chunks

    while(!other_chunk.empty())
    {
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {} events {} - appending chunks", storyId, timelineStart, timelineEnd,
                other_chunk.getStartTime(), other_chunk.getEventCount());
        chunk_to_merge_iter = appendStoryChunk();
        if(chunk_to_merge_iter == storyTimelineMap.end())
        { break; }
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {} into chunk {}", storyId, timelineStart, timelineEnd, 
                other_chunk.getStartTime(), (*chunk_to_merge_iter).second->getStartTime());
        (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
    }

    return;
}
