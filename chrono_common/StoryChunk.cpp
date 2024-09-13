

#include "StoryChunk.h"


namespace chl = chronolog;

/////////////////////////

chl::StoryChunk::StoryChunk(chl::ChronicleName const &chronicle_name, chl::StoryName const &story_name
                            , chl::StoryId const &story_id, uint64_t start_time
                            , uint64_t end_time, uint32_t chunk_size)
                            : chronicleName(chronicle_name), storyName(story_name)
                            , storyId(story_id)
                            , startTime(start_time), endTime(end_time), revisionTime(end_time)
{

}

/////

chl::StoryChunk::~StoryChunk()
{
    logEvents.clear();
}

//////

int chl::StoryChunk::insertEvent(chl::LogEvent const &event)
    {
        if((event.time() >= startTime) && (event.time() < endTime))
        {
            logEvents.insert(std::pair <chl::EventSequence, chl::LogEvent>({event.time(), event.clientId, event.index()}, event));
            return 1;
        }
        else
        { return 0; }
    }

// 
//  merge into this master chunk all the events from the events map startign at iterator position merge_start
//  return the merged even count

uint32_t chl::StoryChunk::mergeEvents(std::map<chl::EventSequence, chl::LogEvent> & events,
                                    std::map<chl::EventSequence, chl::LogEvent>::const_iterator & merge_start)
{
    LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {}-{} : merging map eventCount {}", 
                storyId, startTime, endTime, events.size());

    uint32_t merged_event_count = 0;
    std::map<chl::EventSequence, chl::LogEvent>::const_iterator first_merged, last_merged;

    if( events.empty()) 
    {  return merged_event_count; }


    if((*merge_start).second.time() < startTime)
    {
        merge_start = events.lower_bound(chl::EventSequence{startTime, 0, 0});
        LOG_DEBUG("[StoryChunk] merge StoryId{} master chunk {} : Adjusted merge_start to align with master chunk startTime", 
                            storyId, startTime);
    }

    for(auto iter = merge_start; (iter != events.end()) && ((*iter).second.time() < endTime); ++iter)
    {
        LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {} : merging event {}, master endTime{}", storyId,
                  startTime, (*iter).second.time(), endTime);
        if(insertEvent((*iter).second) > 0)
        {
            if(merged_event_count == 0) 
            { first_merged = iter; }
            last_merged = iter;
            merged_event_count++;
        }
        else
        {
            //stop at the first record that can't be merged
            LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : stopped merging due to event that couldn't be "
                      "inserted.{}",
                      storyId, startTime, (*iter).second.time());
            break;
        }
    }

    if(merged_event_count > 0)
    {
        //remove the merged records from the original map
        // removing records in range [first_merged, last_merged]
        events.erase(first_merged, ++last_merged);
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : merged {} records , remaining map eventCount {}",
                  storyId, startTime, merged_event_count, events.size());
    }
    else
    {
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : No events merged during the operation.", storyId,
                  startTime);
    }

    return merged_event_count;
}

// 
//  merge into this master chunk all the events from the other_chunk with timestamps starting at merge_start_time
//  return the merged even count
  
uint32_t chl::StoryChunk::mergeEvents(chl::StoryChunk & other_chunk, uint64_t merge_start_time)
{
    LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {}-{} : merging chunk {}-{} eventCount {}", storyId, startTime,
              endTime, other_chunk.getStartTime(), other_chunk.getEndTime(), other_chunk.getEventCount());

    uint32_t merged_event_count = 0;

    if( other_chunk.empty()) 
    {  return merged_event_count; }

    if( merge_start_time == 0 || merge_start_time >= other_chunk.getEndTime()) 
    { merge_start_time = other_chunk.getStartTime(); }

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator first_merged, last_merged;

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator merge_start =
            (merge_start_time < startTime ? other_chunk.lower_bound(startTime)
                                          : other_chunk.lower_bound(merge_start_time));

    LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {} : adjusted merge_start : other_chunk {}  merge_start {}",
              storyId, startTime, other_chunk.getStartTime(),
              (merge_start != other_chunk.end() ? (*merge_start).second.time() : (uint64_t)0));

    for(auto iter = merge_start; (iter != other_chunk.end()) && ((*iter).second.time() < endTime); ++iter)
    {
        LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {} : merging event {}, master endTime{}", storyId,
                  startTime, (*iter).second.time(), endTime);
        if(insertEvent((*iter).second) > 0)
        {
            if(merged_event_count == 0) 
            { first_merged = iter; }
            last_merged = iter;
            merged_event_count++;
        }
        else
        {
            //stop at the first record that can't be merged
            LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : stopped merging due to event that couldn't be "
                      "inserted.{}",
                      storyId, startTime, (*iter).second.time());
            break;
        }
    }

    if(merged_event_count > 0)
    {
        //remove the merged records from the original map
        // removing recordis in range [first_merged, last_merged]
        other_chunk.eraseEvents(first_merged, ++last_merged);
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : merged {} records from chunk {} remaining "
                  "eventCount {}",
                  storyId, startTime, merged_event_count, other_chunk.getStartTime(), other_chunk.getEventCount());
    }
    else
    {
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : No events merged during the operation.", storyId,
                  startTime);
    }

    return merged_event_count;
}

/*
uint32_t chl::StoryChunk::extractEvents(std::map <chl::EventSequence, chl::LogEvent> &target_map, std::map <chl::EventSequence, chl::LogEvent>::iterator first_pos
                  , std::map <chl::EventSequence, chl::LogEvent>::iterator last_pos)
    { return 0; }

uint32_t chl::StoryChunk::extractEvents( chl::StoryChunk & target_chunk, uint64_t start_time, uint64_t end_time)
    { return 0; }

*/

//
// remove events falling into range [ range_start, range_end )  
// return iterator to the first element folowing the last removed one

std::map<chl::EventSequence, chl::LogEvent>::iterator
chl::StoryChunk::eraseEvents(std::map<chl::EventSequence, chl::LogEvent>::const_iterator & range_start,
                             std::map<chl::EventSequence, chl::LogEvent>::const_iterator & range_end)
{
    return logEvents.erase(range_start, range_end);
}

//
// remove events falling into range [ start_time, end_time )  
// return iterator to the first element folowing the last removed one

std::map<chl::EventSequence, chl::LogEvent>::iterator chl::StoryChunk::eraseEvents(uint64_t start_time, uint64_t end_time)
{
    if( logEvents.empty() || start_time == 0 || start_time >= end_time || start_time>= endTime || end_time < startTime )
    { return logEvents.end(); }

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator range_start =
            (start_time < startTime ? logEvents.lower_bound(chl::EventSequence{startTime, 0, 0}) 
                                    : logEvents.lower_bound(chl::EventSequence{start_time, 0, 0}));    

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator range_end =
            (end_time > endTime ? logEvents.upper_bound(chl::EventSequence{endTime,0,0}) 
                                : logEvents.upper_bound(chl::EventSequence{end_time,0,0}));
    
    return logEvents.erase(range_start, range_end);
}

///////////////////
