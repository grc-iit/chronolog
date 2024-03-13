

#include "StoryChunk.h"


namespace chl = chronolog;

/////////////////////////

chl::StoryChunk::StoryChunk(chl::StoryId const &story_id , uint64_t start_time , uint64_t end_time , uint32_t chunk_size )
            : storyId(story_id)
            , startTime(start_time)
            , endTime(end_time)
            , revisionTime(start_time)
            , chunkSize(chunk_size)
    {
        dataBlob = new char[chunk_size];
    }

chl::StoryChunk::~StoryChunk()
    {
        delete [] dataBlob;
    }

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

///////////

uint32_t chl::StoryChunk::mergeEvents(std::map <chl::EventSequence, chl::LogEvent> &events
                                            , std::map <chl::EventSequence, chl::LogEvent>::iterator &merge_start)
{
    uint32_t merged_event_count = 0;
    std::map <chl::EventSequence, chl::LogEvent>::iterator first_merged, last_merged;

    if((*merge_start).second.time() < startTime)
    {
        merge_start = events.lower_bound(chl::EventSequence{startTime, 0, 0});
        LOG_DEBUG("[StoryChunk] Adjusted merge start time to align with StoryChunk's start time: {}", startTime);
    }

    for(auto iter = merge_start; iter != events.end(); ++iter)
    {
        if(insertEvent((*iter).second) > 0)
        {
            if(merged_event_count == 0)
            { first_merged = iter; }
            last_merged = iter;
            merged_event_count++;
        }
        else
        {
            LOG_DEBUG("[StoryChunk] Stopped merging due to a record that couldn't be inserted.");
            break;
        }  //stop at the first record that can't be merged
    }

    if(merged_event_count > 0)
    {
        //remove the merged records from the original map
        events.erase(first_merged, last_merged);
        LOG_DEBUG("[StoryChunk] Removed {} merged records from the original event map.", merged_event_count);
    }
    else
    {
        LOG_DEBUG("[StoryChunk] No events merged during the operation.");
    }

    return merged_event_count;
}

uint32_t chl::StoryChunk::mergeEvents(chl::StoryChunk &other_chunk, uint64_t start_time, uint64_t end_time)
    { return 0; }

uint32_t chl::StoryChunk::extractEvents(std::map <chl::EventSequence, chl::LogEvent> &target_map, std::map <chl::EventSequence, chl::LogEvent>::iterator first_pos
                  , std::map <chl::EventSequence, chl::LogEvent>::iterator last_pos)
    { return 0; }

uint32_t chl::StoryChunk::extractEvents(std::map <chl::EventSequence, chl::LogEvent> &target_map, uint64_t start_time, uint64_t end_time)
    { return 0; }

uint32_t chl::StoryChunk::extractEvents( chl::StoryChunk & target_chunk, uint64_t start_time, uint64_t end_time)
    { return 0; }

uint32_t chl::StoryChunk::split(chl::StoryChunk & split_chunk, uint64_t time_boundary)
    { return 0; }


uint32_t chl::StoryChunk::eraseEvents(uint64_t start_time, uint64_t end_time)
    { return 0; }
 
