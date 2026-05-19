#include <chronolog_client.h>

#include <StoryChunk.h>
#include <utility>


namespace chl = chronolog;

/////////////////////////

chl::StoryChunk::StoryChunk(chl::ChronicleName const& chronicle_name,
                            chl::StoryName const& story_name,
                            chl::StoryId const& story_id,
                            uint64_t start_time,
                            uint64_t end_time,
                            uint32_t chunk_size)
    : chronicleName(chronicle_name)
    , storyName(story_name)
    , storyId(story_id)
    , startTime(start_time)
    , endTime(end_time)
    , revisionTime(end_time)
{
    if(endTime <= startTime)
    {
        endTime = (startTime + 5000);
        revisionTime = endTime;
    }
}

/////

chl::StoryChunk::~StoryChunk() { logEvents.clear(); }

//////

int chl::StoryChunk::insertEvent(chl::LogEvent const& event)
{
    if((event.storyId == storyId) && (event.time() >= startTime) && (event.time() < endTime) &&
       !event.logRecord.empty())
    {
        logEvents.insert(
                std::pair<chl::EventSequence, chl::LogEvent>({event.time(), event.clientId, event.index()}, event));
        return 1;
    }
    else
    {
        return 0;
    }
}

int chl::StoryChunk::insertEvent(chl::LogEvent&& event)
{
    if((event.storyId == storyId) && (event.time() >= startTime) && (event.time() < endTime) &&
       !event.logRecord.empty())
    {
        chl::EventSequence key{event.time(), event.clientId, event.index()};
        logEvents.emplace(key, std::move(event));
        return 1;
    }
    else
    {
        return 0;
    }
}

//
//  merge into this master chunk all the events from the events map startign at iterator position merge_start
//  return the merged even count

uint32_t chl::StoryChunk::mergeEvents(std::map<chl::EventSequence, chl::LogEvent>& events,
                                      std::map<chl::EventSequence, chl::LogEvent>::const_iterator& merge_start)
{
    LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {}-{} : merging map eventCount {}",
              storyId,
              startTime,
              endTime,
              events.size());

    uint32_t merged_event_count = 0;
    if(events.empty())
    {
        return merged_event_count;
    }


    if((*merge_start).second.time() < startTime)
    {
        merge_start = events.lower_bound(chl::EventSequence{startTime, 0, 0});
        LOG_DEBUG("[StoryChunk] merge StoryId{} master chunk {} : Adjusted merge_start to align with master chunk "
                  "startTime",
                  storyId,
                  startTime);
    }

    auto iter = merge_start;
    while(iter != events.end() && ((*iter).second.time() < endTime))
    {
        auto current = iter++;
        LOG_TRACE("[StoryChunk] merge StoryId{} master chunk {} : merging event {}, master endTime{}",
                  storyId,
                  startTime,
                  (*current).second.time(),
                  endTime);
        if(((*current).second.storyId == storyId) && ((*current).second.time() >= startTime) &&
           ((*current).second.time() < endTime) && !(*current).second.logRecord.empty())
        {
            auto node = events.extract(current);
            logEvents.insert(std::move(node));
            merged_event_count++;
        }
        /*    else
        {
            //stop at the first record that can't be merged
            LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : stopped merging due to event that couldn't be "
                      "inserted.{}",
                      storyId, startTime, (*iter).second.time());
            break;
        }
*/
    }

    merge_start = iter;
    if(merged_event_count > 0)
    {
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : merged {} records , remaining map eventCount {}",
                  storyId,
                  startTime,
                  merged_event_count,
                  events.size());
    }
    else
    {
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {} : No events merged during the operation.",
                  storyId,
                  startTime);
    }

    return merged_event_count;
}

//
//  merge into this master chunk all the events from the other_chunk with timestamps starting at merge_start_time
//  return the merged even count

uint32_t chl::StoryChunk::mergeEvents(chl::StoryChunk& other_chunk, uint64_t merge_start_time)
{
    LOG_DEBUG("[StoryChunk] merge StoryId{} master chunk {}-{} : merging in chunk {}-{} eventCount {}",
              storyId,
              startTime,
              endTime,
              other_chunk.getStartTime(),
              other_chunk.getEndTime(),
              other_chunk.getEventCount());

    uint32_t merged_event_count = 0;

    if(other_chunk.empty())
    {
        return merged_event_count;
    }

    if(merge_start_time == 0 || merge_start_time >= other_chunk.getEndTime())
    {
        merge_start_time = other_chunk.getStartTime();
    }

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator merge_start =
            (merge_start_time < startTime ? other_chunk.lower_bound(startTime)
                                          : other_chunk.lower_bound(merge_start_time));

    LOG_DEBUG("[StoryChunk] merge StoryId{} master chunk {}-{} : adjusted merge_start : other_chunk {}-{}  merge_start "
              "{}",
              storyId,
              startTime,
              endTime,
              other_chunk.getStartTime(),
              other_chunk.getEndTime(),
              (merge_start != other_chunk.end() ? (*merge_start).second.time() : (uint64_t)0));

    auto iter = merge_start;
    while(iter != other_chunk.end() && ((*iter).second.time() < endTime))
    {
        auto current = iter++;
        LOG_TRACE("[StoryChunk] merge StoryId {} master chunk {}-{} : merging event {} ",
                  storyId,
                  startTime,
                  endTime,
                  (*current).second.time());
        if(((*current).second.storyId == storyId) && ((*current).second.time() >= startTime) &&
           ((*current).second.time() < endTime) && !(*current).second.logRecord.empty())
        {
            auto node = other_chunk.logEvents.extract(current);
            logEvents.insert(std::move(node));
            merged_event_count++;
        }
        /*       else
        {
            //stop at the first record that can't be merged
            LOG_ERROR("[StoryChunk] merge StoryId {} master chunk {}-{} : stopped merging in chunk {}-{} because of event {} ",
                      storyId, startTime,endTime, other_chunk.getStartTime(), other_chunk.getEndTime(), (*iter).second.time());
            break;
        }
*/
    }

    if(merged_event_count > 0)
    {
        LOG_DEBUG("[StoryChunk] merge StoryId {} master chunk {}-{} : merged in {} events from chunk {}-{} remaining "
                  "eventCount {}",
                  storyId,
                  startTime,
                  endTime,
                  merged_event_count,
                  other_chunk.getStartTime(),
                  other_chunk.getEndTime(),
                  other_chunk.getEventCount());
    }
    else
    {
        LOG_DEBUG("[StoryChunk] merge StoryId {} master chunk {}-{} : No events merged in from chunk {}-{}",
                  storyId,
                  startTime,
                  endTime,
                  other_chunk.getStartTime(),
                  other_chunk.getEndTime());
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
chl::StoryChunk::eraseEvents(std::map<chl::EventSequence, chl::LogEvent>::const_iterator& range_start,
                             std::map<chl::EventSequence, chl::LogEvent>::const_iterator& range_end)
{
    return logEvents.erase(range_start, range_end);
}

//
// remove events falling into range [ start_time, end_time )
// return iterator to the first element folowing the last removed one

std::map<chl::EventSequence, chl::LogEvent>::iterator chl::StoryChunk::eraseEvents(uint64_t start_time,
                                                                                   uint64_t end_time)
{
    if(logEvents.empty() || start_time == 0 || start_time >= end_time || start_time >= endTime || end_time < startTime)
    {
        return logEvents.end();
    }

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator range_start =
            (start_time < startTime ? logEvents.lower_bound(chl::EventSequence{startTime, 0, 0})
                                    : logEvents.lower_bound(chl::EventSequence{start_time, 0, 0}));

    std::map<chl::EventSequence, chl::LogEvent>::const_iterator range_end =
            (end_time > endTime ? logEvents.upper_bound(chl::EventSequence{endTime, 0, 0})
                                : logEvents.upper_bound(chl::EventSequence{end_time, 0, 0}));

    return logEvents.erase(range_start, range_end);
}

///////////////////

std::vector<chl::Event>& chl::StoryChunk::extractEventSeries(std::vector<chl::Event>& event_series)
{
    // NOTE: event_series is a vector of chronolog::Event ( client facing event representation)
    // while StoryChunk is using LogEvent (internal chronolog event representation)

    for(auto logEvent: logEvents)
    {
        event_series.push_back(chl::Event{logEvent.second.eventTime,
                                          logEvent.second.clientId,
                                          logEvent.second.eventIndex,
                                          logEvent.second.logRecord});
    }

    logEvents.clear();

    return event_series;
}
