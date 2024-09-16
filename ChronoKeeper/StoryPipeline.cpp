#include <deque>
#include <map>
#include <mutex>


#include "StoryChunk.h"
#include "StoryPipeline.h"
#include "StoryIngestionHandle.h"
#include "StoryChunkExtractionQueue.h"
#include "chrono_monitor.h"

//#define TRACE_CHUNKING
#define TRACE_CHUNK_EXTRACTION

namespace chl = chronolog;

////////////////////////

chronolog::StoryPipeline::StoryPipeline(StoryChunkExtractionQueue &extractionQueue, std::string const &chronicle_name
                                        , std::string const &story_name, chronolog::StoryId const &story_id
                                        , uint64_t story_start_time, uint16_t chunk_granularity
                                        , uint16_t acceptance_window): theExtractionQueue(extractionQueue), storyId(
        story_id), chronicleName(chronicle_name), storyName(story_name), timelineStart(story_start_time), timelineEnd(
        story_start_time), chunkGranularity(chunk_granularity), acceptanceWindow(acceptance_window)
                                                                       , activeIngestionHandle(nullptr)
{
    activeIngestionHandle = new chl::StoryIngestionHandle(ingestionMutex, &eventQueue1, &eventQueue2);

    //pre-initialize the pipeline map with the StoryChunks of chunkGranulary 
    // with the total timelength of at least 2 chunks (merging logic assumes at least 2 chunks in the active pipeline)

    auto story_start_point = std::chrono::time_point <std::chrono::system_clock, std::chrono::nanoseconds>{} +
                             std::chrono::nanoseconds(timelineStart);
    std::time_t time_t_story_start = std::chrono::high_resolution_clock::to_time_t(story_start_point);
    LOG_INFO("[StoryPipeline] Initialized with StoryID={}, StoryStartTime={}, Chronology={}, "
             "ChunkGranularity={} seconds, AcceptanceWindow={} seconds", storyId, std::ctime(
            &time_t_story_start), chronicleName, chunkGranularity / 1000000000, acceptanceWindow / 1000000000);

    chunkGranularity *= 1000000000;    // seconds =>nanoseconds
    acceptanceWindow *= 1000000000;    // seconds =>nanoseconds

    //adjust the timelineStart to the closest prior boundary of chunkGranularity
    timelineStart -= (timelineStart % chunkGranularity);
    timelineEnd = timelineStart;

    while( timelineEnd < timelineStart + chunkGranularity * 3)
    {
        appendStoryChunk();
    }

#ifdef TRACE_CHUNKING
    auto chunk_start_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{} // epoch_time_point{};
        + std::chrono::nanoseconds(timelineStart);
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{}
        + std::chrono::nanoseconds(timelineEnd);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point); 
    LOG_DEBUG("[StoryPipeline] Created StoryPipeline with StoryID={}, StartChunk={}, EndChunk={}",
                               storyId, std::ctime(&time_t_chunk_start), std::ctime(&time_t_chunk_end));
#endif

}
///////////////////////

chl::StoryIngestionHandle*chl::StoryPipeline::getActiveIngestionHandle()
{
    return activeIngestionHandle;
}

///////////////////////
chronolog::StoryPipeline::~StoryPipeline()
{
    LOG_DEBUG("[StoryPipeline] Destructor called for StoryID={}", storyId);
    finalize();
}
///////////////////////

void chronolog::StoryPipeline::finalize()
{
    //by this time activeIngestionHandle is disengaged from the IngestionQueue 
    // as part of KeeperDataStore::shutdown
    if(activeIngestionHandle != nullptr)
    {
        if(!activeIngestionHandle->getPassiveDeque().empty())
        {
            mergeEvents(activeIngestionHandle->getPassiveDeque());
        }
        if(!activeIngestionHandle->getActiveDeque().empty())
        {
            mergeEvents(activeIngestionHandle->getActiveDeque());
        }
        delete activeIngestionHandle;
        LOG_INFO("[StoryPipeline] Finalized ingestion handle for storyId: {}", storyId);
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

#ifdef TRACE_CHUNK_EXTRACTION
            LOG_TRACE("[StoryPipeline] Finalized chunk for StoryID={}. Is empty: {}", storyId, extractedChunk->empty()
                                                                                          ? "Yes" : "No");
#endif
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
    LOG_TRACE("[StoryPipeline] Prepending new chunk for StoryID={} starting at: {}",
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
    LOG_TRACE("[StoryPipeline] Appending new chunk for StoryID={} starting at: {}",
                               storyId, timelineEnd);
#endif
    auto result = storyTimelineMap.insert(
            std::pair <uint64_t, chronolog::StoryChunk*>(timelineEnd, new chronolog::StoryChunk(chronicleName, storyName
                                                                                                , storyId
                                                                                                , timelineEnd, timelineEnd + chunkGranularity)));
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
    if(!activeIngestionHandle->getPassiveDeque().empty())
    {
        mergeEvents(activeIngestionHandle->getPassiveDeque());
    }
    LOG_INFO("[StoryPipeline] Collected ingested events for StoryID={}", storyId);
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
    LOG_TRACE("[StoryPipeline] StoryID: {} - Current time: {} - Timeline size: {} - Head chunk decay time: {}", storyId
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
#ifdef TRACE_CHUNK_EXTRACTION
            LOG_TRACE("[StoryPipeline] StoryID: {} - Extracted chunk with start time {} is empty: {}", storyId
                 , extractedChunk->getStartTime(), (extractedChunk->empty() ? "Yes" : "No"));
#endif
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
#ifdef TRACE_CHUNK_EXTRACTION
    LOG_TRACE("[StoryPipeline] Extracting decayed chunks for StoryID={}. Queue size: {}", storyId
         , theExtractionQueue.size());
#endif
}

////////////////////

void chronolog::StoryPipeline::mergeEvents(chronolog::EventDeque &event_deque)
{
    if(event_deque.empty())
    { return; }

    std::lock_guard <std::mutex> lock(sequencingMutex);
    chl::LogEvent event;
    // the last chunk is most likely the one that would get the events, so we'd start with the last
    // chunk and do the lookup only if it's not the one
    // NOTE: we should never have less than 2 chunks in the active storyTimelineMap !!!
    std::map <uint64_t, chronolog::StoryChunk*>::iterator chunk_to_merge_iter = --storyTimelineMap.end();
    while(!event_deque.empty())
    {
        event = event_deque.front();
        LOG_DEBUG("[StoryPipeline] StoryID: {} [Start: {}, End: {}]: Merging event time: {}", storyId, timelineStart
             , timelineEnd, event.time());
        if(timelineStart <= event.time() && event.time() < timelineEnd)
        {
            // we expect the events in the deque to be mostly monotonous
            // so we'd try the most recently used chunk first and only look for the new chunk
            // if the event does not belong to the recently used chunk
            if(!(*chunk_to_merge_iter).second->insertEvent(event))
            {
                // find the new chunk_to_merge the event into : we are lookingt for
                // the chunk preceeding the first chunk with the startTime > event.time()
                chunk_to_merge_iter = storyTimelineMap.upper_bound(event.time());
                //merge into the preceeding chunk
                if(!(*(--chunk_to_merge_iter)).second->insertEvent(event))
                {
                    LOG_ERROR("[StoryPipeline] StoryID: {} - Discarded event with timestamp: {}", storyId, event.time());
                }
            }
        }
        else if(event.time() >= timelineEnd)
        {  //extend timeline forward
            while(event.time() >= timelineEnd)
            {
                chunk_to_merge_iter = appendStoryChunk();
                if(chunk_to_merge_iter == storyTimelineMap.end())
                { break; }
            }
            if(chunk_to_merge_iter != storyTimelineMap.end())
            { (*chunk_to_merge_iter).second->insertEvent(event); }
            else
            {
                LOG_ERROR("[StoryPipeline] StoryID: {} - Discarding event with timestamp: {}", storyId, event.time());
            }
        }
        else
        {  //extend timeline backward
            while(event.time() < timelineStart)
            {
                chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
                if(chunk_to_merge_iter == storyTimelineMap.end())
                { break; }
            }
            if(chunk_to_merge_iter != storyTimelineMap.end())
            { (*chunk_to_merge_iter).second->insertEvent(event); }
            else
            {
                LOG_ERROR("[StoryPipeline] StoryID: {} - Discarding event with timestamp: {}", storyId, event.time());
            }
        }
        event_deque.pop_front();
    }
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

    LOG_DEBUG("[StoryPipeline] StoryID: {} - Merging StoryChunk from {} to {}", storyId, other_chunk.getStartTime()
         , other_chunk.getEndTime());

    // locate the storyChunk in the StoryPipeline with the time Key not less than
    // other_chunk.startTime and start merging
    std::map <uint64_t, chronolog::StoryChunk*>::iterator chunk_to_merge_iter;

    if(timelineStart <= other_chunk.getStartTime())
    {
        // find the chunk_to_merge into : we are lookingt for
        // the chunk preceeding the one with the startTime > other_chunk.getStartTime()
        chunk_to_merge_iter = --storyTimelineMap.upper_bound(other_chunk.getStartTime());
    }
    else
    {
        // unlikely but possible that we get some delayed events and need to prepend some chunks
        // extending the timeline back to the past
        LOG_DEBUG("[StoryPipeline] StoryID: {} - Prepending merge starting at timestamp {}", storyId
             , other_chunk.getStartTime());
        while(timelineStart > other_chunk.getStartTime())
        {
            chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
            if(chunk_to_merge_iter == storyTimelineMap.end())
            {
                //INNA:: if prepend fails we have no choice but to discard the events we can't merge !!
                LOG_ERROR("[StoryPipeline] StoryID: {} - Merge operation discards events between timestamps: {} and {}"
                     , storyId, other_chunk.getStartTime(), timelineStart);
                other_chunk.eraseEvents(other_chunk.getStartTime(), timelineStart);
                chunk_to_merge_iter = storyTimelineMap.begin();
            }
        }
    }

    //iterate through the storyTimelineMap draining the other_chunk events
    while(chunk_to_merge_iter != storyTimelineMap.end() && !other_chunk.empty())
    {
        (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
        chunk_to_merge_iter++;
    }

    // if there are still records in the other_chunk with the timestamps beyond the current timelineEnd
    // we extend the timeline forward by appending new chunks

    while(!other_chunk.empty())
    {
        chunk_to_merge_iter = appendStoryChunk();
        if(chunk_to_merge_iter == storyTimelineMap.end())
        { break; }

        (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
    }

    return;
}
