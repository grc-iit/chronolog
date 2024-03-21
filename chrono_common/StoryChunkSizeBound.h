#ifndef STORY_CHUNK_H
#define STORY_CHUNK_H

#include <set>
#include <map>
#include <iostream>
#include "chronolog_types.h"
#include "log.h"

namespace chronolog
{

typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

// StoryChunk contains all the events for the single story
// for the duration [startTime, endTime[
// startTime included, endTime excluded
// startTime/endTime are invariant

typedef std::tuple <chrono_time, chrono_index> ArrivalSequence;

typedef std::tuple <chrono_time, ClientId, chrono_index> EventSequence;

class StoryChunk
{
public:

    StoryChunk(StoryId const &story_id = 0, uint64_t start_time = 0, uint64_t end_time = 0, uint16_t chunk_size = 1024);

    ~StoryChunk();

    StoryId const &getStoryId() const
    { return storyId; }

    uint64_t getStartTime() const
    { return startTime; }

    uint64_t getEndTime() const
    { return endTime; }

    bool empty() const
    { return (logEvents.empty() ? true : false); }

    std::map <EventSequence, LogEvent>::const_iterator begin() const
    { return logEvents.begin(); }

    std::map <EventSequence, LogEvent>::const_iterator end() const
    { return logEvents.end(); }

    std::map <EventSequence, LogEvent>::const_iterator lower_bound(uint64_t chrono_time) const
    { return logEvents.lower_bound(EventSequence{chrono_time, 0, 0}); }

    uint64_t firstEventTime() const
    { return (*logEvents.begin()).second.time(); }
    
    uint64_t lastEventTime() const
    { return (*logEvents.begin()).second.time(); }

    bool is_chunk_full() const 
    { return ( free_space == 0); }

    uint16_t get_used_space() const
    { return used_space; }

    uint16_t get_free_space() const
    {   return free_space; }

    bool timestamp_within_range( uint64_t timestamp) const
    {   return (( startTime <= timestamp) && (timestamp < endTime)); }


//    std::set<EventSequence> & getKeys(std::set<EventSequence> &) const;

  //  std::ostream & streamKeys( std::ostream &);
    //std::ostream & streamContents( std::ostream &);

    int insertEvent(LogEvent const &);
    
    uint32_t
    mergeEvents(std::map <EventSequence, LogEvent> &events, std::map <EventSequence, LogEvent>::iterator &merge_start);

    uint32_t mergeEvents(StoryChunk &other_chunk, uint64_t start_time =0, uint64_t end_time=0);

    uint32_t
    extractEvents(std::map <EventSequence, LogEvent> &target_map, std::map <EventSequence, LogEvent>::iterator first_pos
                  , std::map <EventSequence, LogEvent>::iterator last_pos);

    uint32_t extractEvents(std::map <EventSequence, LogEvent> &target_map, uint64_t start_time, uint64_t end_time);

    uint32_t extractEvents( StoryChunk & target_chunk, uint64_t start_time, uint64_t end_time);

    uint32_t moveEvents(StoryChunk & target, uint64_t start_time, uint64_t end_time);
    uint32_t split(uint64_t time_boundary, StoryChunk &); //this is really a subset of moveEvents...

    uint32_t eraseEvents(uint64_t start_time, uint64_t end_time);

private:

    int appendLogRecordToBlob( uint8_t* record_ptr, uint16_t record_length);
    // move record from one ChunktoTheOther
    // extractLogRecordFromBlob( sequence, std::string& log_record); read the recod from blob
    // removeLogRecordFromBlob( sequence, offset) ; // remove the offset from map & move the used_space pointer back if the record is the last one in the blob
    
    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;
    uint16_t free_space;
    uint16_t used_space;
    uint8_t * dataBlob;
    std::map <EventSequence, uint16_t > eventOffsetMap;
    std::map <EventSequence, LogEvent> logEvents;
};


/*inline std::ostream & operator<< (std::ostream & out, StoryChunk const& chunk)
{

return chunk.streamContents(out);
}*/
}
#endif
