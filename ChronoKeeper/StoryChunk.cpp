

#include "StoryChunk.h"


namespace chl = chronolog;

/////////////////////////

uint32_t chronolog::StoryChunk::mergeEvents(std::map <chl::EventSequence, chl::LogEvent> &events
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

