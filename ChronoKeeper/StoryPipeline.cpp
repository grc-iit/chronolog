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
                        , uint16_t acceptance_window)
    : theExtractionQueue(extractionQueue), storyId(story_id)
    , chronicleName(chronicle_name), storyName(story_name)
    , chunkGranularity(chunk_granularity), acceptanceWindow(acceptance_window)
    , activeIngestionHandle(nullptr)
{
    activeIngestionHandle = new chl::StoryIngestionHandle(ingestionMutex, &eventQueue1, &eventQueue2);

    //pre-initialize the pipeline map with the StoryChunks of chunkGranulary 
    // with the total timelength of at least 2 chunks (merging logic assumes at least 2 chunks in the active pipeline)

    chunkGranularity *= 1000000000;    // seconds =>nanoseconds
    acceptanceWindow *= 1000000000;    // seconds =>nanoseconds

    //adjust the timeline start to the closest prior boundary of chunkGranularity
    story_start_time -= (story_start_time % chunkGranularity);

    for(int i=0; i<3; ++i)
    {
        StoryChunk * new_chunk = new chronolog::StoryChunk(chronicleName, storyName, storyId, (story_start_time + chunkGranularity*i), (story_start_time + chunkGranularity*(i+1)));
        storyTimelineMap.insert( std::pair <uint64_t, chronolog::StoryChunk*>(new_chunk->getStartTime(), new_chunk));
    }

    auto timeline_start_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{} // epoch_time_point{};
        + std::chrono::nanoseconds(TimelineStart());
    std::time_t time_t_story_start = std::chrono::high_resolution_clock::to_time_t(timeline_start_point);
    auto timeline_end_point = std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds>{}
        + std::chrono::nanoseconds(TimelineEnd());
    std::time_t time_t_story_end = std::chrono::high_resolution_clock::to_time_t(timeline_end_point);
    LOG_INFO("[StoryPipeline] Initialized StoryPipleine StoryID={}, {}-{} timeline {}-{} Start {} End {} "
             "ChunkGranularity={} seconds, AcceptanceWindow={} seconds", storyId, chronicleName, storyName, TimelineStart(), TimelineEnd()
            , std::ctime(&time_t_story_start), std::ctime(&time_t_story_end), chunkGranularity / 1000000000, acceptanceWindow / 1000000000);

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
            LOG_TRACE("[StoryPipeline] Finalized chunk for StoryID={}. Is empty: {}", storyId, (extractedChunk->empty() ? "Yes" : "No"));
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
    auto chunk_start_point = epoch_time_point + std::chrono::nanoseconds(TimelineStart()-chunkGranularity);
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = epoch_time_point + std::chrono::nanoseconds(TimelineStart());
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point);
    LOG_TRACE("[StoryPipeline] Prepending new chunk for StoryID={} timeline {}-{} prepend_chunk {}-{}",
                               storyId, TimelineStart(), TimelineEnd(), std::ctime(&time_t_chunk_start), std::ctime(&time_t_chunk_end));
#endif
    auto result = storyTimelineMap.insert(
            std::pair <uint64_t, chronolog::StoryChunk*>(TimelineStart() - chunkGranularity, new chronolog::StoryChunk(
                    chronicleName, storyName, storyId, TimelineStart() - chunkGranularity, TimelineStart())));
    if(!result.second)
    {
        return storyTimelineMap.end();
    }
    else
    {
        return result.first;
    }
}
/////////////////////////////

std::map <uint64_t, chronolog::StoryChunk*>::iterator chronolog::StoryPipeline::appendStoryChunk()
{
    // append the next storyChunk at the end of storyTimeline and return the iterator to the new node
#ifdef TRACE_CHUNKING
    std::chrono::time_point<std::chrono::system_clock,std::chrono::nanoseconds> epoch_time_point{};
    auto chunk_start_point = epoch_time_point + std::chrono::nanoseconds(TimelineEnd());
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = epoch_time_point + std::chrono::nanoseconds(TimelineEnd()+chunkGranularity);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point);
    LOG_TRACE("[StoryPipeline] Appending new chunk for StoryID={} timeline {}-{} new_chunk {}-{}",
                               storyId, TimelineStart(), TimelineEnd(), time_t_chunk_start,time_t_chunk_end);
#endif
    auto result = storyTimelineMap.insert(
            std::pair <uint64_t, chronolog::StoryChunk*>(TimelineEnd(), new chronolog::StoryChunk(chronicleName, storyName
                                                                                                , storyId
                                                                                                , TimelineEnd(), TimelineEnd() + chunkGranularity)));
    if(!result.second)
    {
        return storyTimelineMap.end();
    }
    else
    {
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
        LOG_DEBUG("[StoryPipeline] StoryID: {} [Start: {}, End: {}]: Merging event time: {}", storyId, TimelineStart()
             , TimelineEnd(), event.time());
        if(TimelineStart() <= event.time() && event.time() < TimelineEnd())
        {
            // we expect the events in the deque to be mostly monotonous
            // so we'd try the most recently used chunk first and only look for the new chunk
            // if the event does not belong to the recently used chunk
            if(!(*chunk_to_merge_iter).second->insertEvent(event))
            {
                // find the new chunk_to_merge the event into : we are lookingt for
                // the chunk preceeding the first chunk with the startTime > event.time()
                chunk_to_merge_iter = storyTimelineMap.lower_bound(event.time());  
                // in a rare case when the event.time() is exactly on the chunk.StartTime boundary merge into it right away,
                //  otherwise merge into the preceeding chunk
                if(chunk_to_merge_iter == storyTimelineMap.end() || (*chunk_to_merge_iter).second->getStartTime() > event.time())
                { 
                  chunk_to_merge_iter --;
                }
                
                if(!(*chunk_to_merge_iter).second->insertEvent(event))
                {
                    LOG_ERROR("[StoryPipeline] StoryID: {} - Discarded event with timestamp: {}", storyId, event.time());
                }
            }
        }
        else if(event.time() >= TimelineEnd())
        {  //extend timeline forward
            while(event.time() >= TimelineEnd())
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
        {  
            // a case of seriously delayed event that arrives at the ChronoKeeper after the StoryChunk it belongs to has already been extracted from the StoryPipeline
            // we need (1) to extend the pipeline into the past by prepending the story chunks and (2) to increase the acceptance window so that we don't have to keep prepending story chunks for the slow client - chrono_keeper connection case...
             LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : delayed event {} - need to prepend chunks ", storyId, TimelineStart(), TimelineEnd(), event.time());

            //we also increase the acceptance window for the slow story environment so that we do not have to keep prepending StoryChunks 
            // and deal with auxiliary files if recording of this particular Story turns to be slow...
            acceptanceWindow = (TimelineStart() - event.time()) + 3000; //current delay + 3 microseconds buffer

            LOG_INFO("[StoryPipeline] StoryId {} timeline {}-{} : increased acceptanceWindow to {} seconds", storyId, TimelineStart(), TimelineEnd(), acceptanceWindow/1000000000);

            while(event.time() < TimelineStart())
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
