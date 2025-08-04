#include <deque>
#include <map>
#include <mutex>

#include <chrono_monitor.h>
#include <StoryChunk.h>
#include <StoryPipeline.h>
#include <StoryChunkIngestionHandle.h>
#include <StoryChunkExtractionQueue.h>

//#define TRACE_CHUNKING
//#define TRACE_CHUNK_EXTRACTION
#define TRACE_MERGING 1

namespace chl = chronolog;

////////////////////////

chronolog::StoryPipeline::StoryPipeline(StoryChunkExtractionQueue &extractionQueue, chronolog::ChronicleName const &chronicle_name
                                        , chronolog::StoryName const &story_name, chronolog::StoryId const &story_id
                                        , uint64_t story_start_time, uint32_t chunk_granularity
                                        , uint32_t acceptance_window)
        : theExtractionQueue(extractionQueue)
        , storyId(story_id), chronicleName(chronicle_name), storyName(story_name)
        , chunkGranularity(chunk_granularity), acceptanceWindow(acceptance_window)
        , activeIngestionHandle(nullptr)
{
    activeIngestionHandle = new chl::StoryChunkIngestionHandle(ingestionMutex, &chunkQueue1, &chunkQueue2);

    //pre-initialize the pipeline map with the StoryChunks of chunkGranulary 
    // (merging logic assumes at least 2 chunks in the active pipeline)

    chunkGranularity *= 1000000000;    // seconds =>nanoseconds
    acceptanceWindow *= 1000000000;    // seconds =>nanoseconds

    //adjust the timeline start to the closest previous boundary of chunkGranularity
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
    auto chunk_start_point = epoch_time_point + std::chrono::nanoseconds(TimelineStart());
    std::time_t time_t_chunk_start = std::chrono::high_resolution_clock::to_time_t(chunk_start_point);
    auto chunk_end_point = epoch_time_point + std::chrono::nanoseconds(TimelineStart()-chunkGranularity);
    std::time_t time_t_chunk_end = std::chrono::high_resolution_clock::to_time_t(chunk_end_point);
    LOG_TRACE("[StoryPipeline] Prepending new chunk for StoryId {} timeline {}-{} ", storyId, TimelineStart(), TimelineEnd());
#endif
    StoryChunk * new_chunk = new chronolog::StoryChunk(chronicleName, storyName, storyId, TimelineStart() - chunkGranularity, TimelineStart());

    auto result = storyTimelineMap.insert( std::pair <uint64_t, chronolog::StoryChunk*>(new_chunk->getStartTime(), new_chunk));

    if(!result.second)
    {
        delete new_chunk;
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
    LOG_TRACE("[StoryPipeline] Appending new chunk for StoryId {}  timeline {}-{}", storyId, TimelineStart(), TimelineEnd());

#endif

    chl::StoryChunk * new_chunk = new chronolog::StoryChunk(chronicleName, storyName, storyId, TimelineEnd(),TimelineEnd() + chunkGranularity);
    auto result = storyTimelineMap.insert( std::pair <uint64_t, chronolog::StoryChunk*>(TimelineEnd(),new_chunk));

    if(!result.second)
    {
        delete new_chunk;
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
    LOG_TRACE("[StoryPipeline] StoryId: {} - Current time: {} - TimelineMap-size: {} - Head chunk decay time: {}", storyId
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

    LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} eventCount {} 1stEventTime {}", storyId, TimelineStart(), TimelineEnd()
            ,  other_chunk.getStartTime(), other_chunk.getEndTime(), other_chunk.getEventCount(), other_chunk.firstEventTime());

    // locate the storyChunk in the existing StoryPipeline with the time Key not less than
    // other_chunk.startTime, and start merging 
    // or extend the StoryPipeline in time so that the other chunk can be merged in

    std::map <uint64_t, chronolog::StoryChunk*>::iterator chunk_to_merge_iter;

    if(other_chunk.firstEventTime() < TimelineStart())
    {
        // it's unlikely but possible that we get some delayed events and need to prepend some chunks
        // extending the timeline back into the past
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} 1stEventTime {} - need to prepend chunks ", storyId, TimelineStart(), TimelineEnd(), other_chunk.getStartTime(), other_chunk.getEndTime(),other_chunk.firstEventTime());

        //we also increase the acceptance window for the slow story environment so that we do not have to keep prepending StoryChunks 
        // and deal with auxiliary files if recording of this particular Story turns to be slow...
        acceptanceWindow = (TimelineStart() - other_chunk.firstEventTime()) + 3000; //current delay + 3 microseconds buffer

        LOG_INFO("[StoryPipeline] StoryId {} timeline {}-{} : increasing acceptanceWindow to {} seconds", storyId, TimelineStart(), TimelineEnd(), acceptanceWindow/1000000000);

        while(other_chunk.firstEventTime() < TimelineStart())
        {
            chunk_to_merge_iter = chl::StoryPipeline::prependStoryChunk();
            if(chunk_to_merge_iter == storyTimelineMap.end())
            {
                // if prepend fails we have no choice but to discard the events we can't merge !!
                LOG_ERROR("[StoryPipeline] StoryId {} timeline {}-{} : Merging operation discards events between timestamps: {} and {}"
                     , storyId, TimelineStart(), TimelineEnd(), other_chunk.getStartTime(), TimelineStart());
                other_chunk.eraseEvents(other_chunk.firstEventTime(), TimelineStart());
                chunk_to_merge_iter = storyTimelineMap.begin();
            }
        }
        
    }
    else if((other_chunk.firstEventTime() >= TimelineStart()) && (other_chunk.firstEventTime() < TimelineEnd()))
    {
        // we are looking for the lower_bound StoryChunk that is the chunk with the StartTime equal or greater than other_chunk-getStartTime()
        
        chunk_to_merge_iter = storyTimelineMap.lower_bound(other_chunk.firstEventTime());
        
        if(chunk_to_merge_iter == storyTimelineMap.end())
        {
            // the last chunk is the one to merge into because we know that other_chunk.firstEventTime() is within TimelineStart/End boundaries
            chunk_to_merge_iter--;
            LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} into the last chunk {}-{}", storyId, TimelineStart(), TimelineEnd(),
                    other_chunk.getStartTime(), other_chunk.getEndTime(), (*chunk_to_merge_iter).second->getStartTime(), (*chunk_to_merge_iter).second->getEndTime());
            
        }
        else if( other_chunk.firstEventTime() == (*chunk_to_merge_iter).second->getStartTime())
        {
          // we start merging with chunk_to_merge_iter
        
            LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} starts with chunk {} - exact lower_bound", storyId, TimelineStart(), TimelineEnd(), other_chunk.getStartTime(), other_chunk.getEndTime(), (*chunk_to_merge_iter).second->getStartTime());
        }
        else if( other_chunk.firstEventTime() < (*chunk_to_merge_iter).second->getStartTime())
        {
            // we are looking for the chunk preceeding the lower bound chunk

            if(chunk_to_merge_iter != storyTimelineMap.begin())
            {
                chunk_to_merge_iter--;
                LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} starts with chunk {} preceeeds lower_bound", storyId, TimelineStart(), TimelineEnd(), other_chunk.getStartTime(), other_chunk.getEndTime(), (*chunk_to_merge_iter).second->getStartTime());
            }
            else
            {
                LOG_ERROR("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} - attempt to decrement map.begin", storyId, TimelineStart(), TimelineEnd(),
                    other_chunk.getStartTime(), other_chunk.getEndTime());
            }
        } 
    }
    else //case other_chunk.getFirstEventTime() > TimelineEnd()
    {
        chunk_to_merge_iter = storyTimelineMap.end();
    }


    // if we found a valid StoryChunk in the pipeline to merge into start merging... 
    // iterate through the storyTimelineMap draining the other_chunk events
    while(chunk_to_merge_iter != storyTimelineMap.end() && !other_chunk.empty() && (other_chunk.firstEventTime() < TimelineEnd()))
    {
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} into chunk {}-{}", storyId, TimelineStart(), TimelineEnd(), 
                other_chunk.getStartTime(),other_chunk.getEndTime(), (*chunk_to_merge_iter).second->getStartTime(),(*chunk_to_merge_iter).second->getEndTime());
        (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
        chunk_to_merge_iter++;
    }

    // if there are still records in the other_chunk with the timestamps beyond the current timeline End
    // we extend the timeline forward by appending new chunks

    while(!other_chunk.empty() && (other_chunk.getEndTime() >= TimelineEnd()))
    {
        LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging StoryChunk {}-{} events {} - appending chunks", storyId, TimelineStart(), TimelineEnd(),
                other_chunk.getStartTime(), other_chunk.getEndTime(), other_chunk.getEventCount());
        chunk_to_merge_iter = appendStoryChunk();
        if(chunk_to_merge_iter == storyTimelineMap.end())
        { break; }
        else if( other_chunk.firstEventTime() < TimelineEnd())
        { 
            LOG_DEBUG("[StoryPipeline] StoryId {} timeline {}-{} : Merging in StoryChunk {}-{} into chunk {}-{}", storyId, TimelineStart(), TimelineEnd(), 
                other_chunk.getStartTime(),other_chunk.getEndTime(), (*chunk_to_merge_iter).second->getStartTime(),(*chunk_to_merge_iter).second->getEndTime());
            (*chunk_to_merge_iter).second->mergeEvents(other_chunk);
        }
    }


    if(!other_chunk.empty())
    {
        // if merging fails we have no choice but to discard the events we can't merge !!
        LOG_ERROR("[StoryPipeline] StoryId {} timeline {}-{} : Merge operation discards {} events in chunk {}-{}"
                     , storyId, TimelineStart(), TimelineEnd(), other_chunk.getEventCount(), other_chunk.getStartTime(), other_chunk.getEndTime());
#ifdef TRACE_MERGING
        LOG_ERROR("[StoryPipeline] StoryId {} timeline {}-{} : Merge operation discards {} events in chunk {}"
                     , storyId, TimelineStart(), TimelineEnd(), other_chunk.getEventCount(), other_chunk.to_string());
#endif
                other_chunk.eraseEvents(other_chunk.getStartTime(), other_chunk.getEndTime());
    }

    return;
}
