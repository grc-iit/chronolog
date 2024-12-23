#ifndef STORY_CHUNK_H
#define STORY_CHUNK_H

#include <map>
#include <iostream>
#include <sstream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <thallium/serialization/stl/tuple.hpp>
#include "chronolog_types.h"
#include "chrono_monitor.h"

namespace chronolog
{

// StoryChunk contains all the events for the single story
// for the duration [startTime, endTime[
// startTime included, endTime excluded
// startTime/endTime are invariant

typedef std::tuple <chrono_time, chrono_index> ArrivalSequence;

typedef std::tuple <chrono_time, ClientId, chrono_index> EventSequence;

typedef std::tuple <uint64_t, uint64_t> EventOffsetSize;

class StoryChunk
{
public:

    StoryChunk(ChronicleName const &chronicle_name = "", StoryName const &story_name = ""
               , StoryId const &story_id = 0, uint64_t start_time = 0, uint64_t end_time = 0
               , uint32_t chunk_size = 1024);

    ~StoryChunk();

    ChronicleName const &getChronicleName() const
    { return chronicleName; }

    StoryName const &getStoryName() const
    { return storyName; }

    StoryId const &getStoryId() const
    { return storyId; }

    uint64_t getStartTime() const
    { return startTime; }

    uint64_t getEndTime() const
    { return endTime; }

    int getEventCount() const
    { return logEvents.size(); }

    bool empty() const
    { return (logEvents.empty() ? true : false); }

    std::map <EventSequence, LogEvent>::const_iterator begin() const
    { return logEvents.begin(); }

    std::map <EventSequence, LogEvent>::const_iterator end() const
    { return logEvents.end(); }

    std::map <EventSequence, LogEvent>::const_iterator lower_bound(uint64_t chrono_time) const
    { return logEvents.lower_bound(EventSequence{chrono_time, 0, 0}); }

    uint64_t firstEventTime() const
    { return (logEvents.empty() ? 0 : (*logEvents.begin()).second.time()); }

    uint64_t lastEventTime() const
    { return (logEvents.empty() ? 0 : (*(--logEvents.end())).second.time()); }

    int insertEvent(LogEvent const &);

    uint32_t mergeEvents(std::map <EventSequence, LogEvent> &events
                         , std::map <EventSequence, LogEvent>::const_iterator &merge_start);

    uint32_t mergeEvents(StoryChunk &other_chunk, uint64_t start_time = 0);

    /*    uint32_t
    extractEvents(std::map <EventSequence, LogEvent> &target_map, std::map <EventSequence, LogEvent>::iterator first_pos
                  , std::map <EventSequence, LogEvent>::iterator last_pos);

    uint32_t extractEvents(std::map <EventSequence, LogEvent> &target_map, uint64_t start_time, uint64_t end_time);

    uint32_t extractEvents( StoryChunk & target_chunk, uint64_t start_time, uint64_t end_time);
*/
    std::map <EventSequence, LogEvent>::iterator
    eraseEvents(std::map <EventSequence, LogEvent>::const_iterator &first_pos
                , std::map <EventSequence, LogEvent>::const_iterator &last_pos);

    std::map <EventSequence, LogEvent>::iterator eraseEvents(uint64_t start_time, uint64_t end_time);

    // serialization function used by thallium RPC providers
    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT&chronicleName;
        serT&storyName;
        serT&storyId;
        serT&startTime;
        serT&endTime;
        serT&revisionTime;
        serT&logEvents;
    }

private:
    ChronicleName chronicleName;
    StoryName storyName;
    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;
    std::map <EventSequence, LogEvent> logEvents;
};

class StoryChunkHVL
{
public:
    explicit StoryChunkHVL(ChronicleName const &chronicle_name = "", StoryName const &story_name = ""
                           , StoryId const &story_id = 0, uint64_t start_time = 0
                           , uint64_t end_time = 0, uint32_t chunk_size = 0)
            : chronicleName(chronicle_name), storyName(story_name)
              , storyId(story_id)
              , startTime(start_time), endTime(end_time), revisionTime(end_time)
    {}

    ~StoryChunkHVL() = default;

    ChronicleName const &getChronicleName() const
    { return chronicleName; }

    StoryName const &getStoryName() const
    { return storyName; }

    [[nodiscard]] StoryId const &getStoryId() const
    { return storyId; }

    [[nodiscard]] uint64_t getStartTime() const
    { return startTime; }

    [[nodiscard]] uint64_t getEndTime() const
    { return endTime; }

    [[nodiscard]] bool empty() const
    { return logEvents.empty(); }

    [[nodiscard]] size_t getEventCount() const
    { return logEvents.size(); }

    [[nodiscard]] size_t getTotalPayloadSize() const
    {
        size_t total_size = 0;
        for(auto const &event: logEvents)
        { total_size += event.second.logRecord.len; }
        return total_size;
    }

    [[nodiscard]] std::map <EventSequence, LogEventHVL>::const_iterator begin() const
    { return logEvents.begin(); }

    [[nodiscard]] std::map <EventSequence, LogEventHVL>::const_iterator end() const
    { return logEvents.end(); }

    [[nodiscard]] std::map <EventSequence, LogEventHVL>::const_iterator lower_bound(uint64_t chrono_time) const
    { return logEvents.lower_bound(EventSequence{chrono_time, 0, 0}); }

    [[nodiscard]] uint64_t firstEventTime() const
    { return (*logEvents.begin()).second.time(); }

    int insertEvent(LogEventHVL const &event)
    {
        if((event.time() >= startTime) && (event.time() < endTime))
        {
            logEvents.insert(std::pair <EventSequence, LogEventHVL>({event.time(), event.clientId, event.index()},
                                                                  event));
            return 1;
        }
        else
        { return 0; }
    }

    bool operator==(const StoryChunkHVL &other) const
    {
        return ((storyId == other.storyId) && (startTime == other.startTime) && (endTime == other.endTime) &&
                (revisionTime == other.revisionTime) && (logEvents == other.logEvents));
    }

    bool operator!=(const StoryChunkHVL &other) const
    {
        return !(*this == other);
    }

    [[nodiscard]] std::string toString() const
    {
        std::stringstream ss;
        ss << "StoryChunk:{" << storyId << ":" << startTime << ":" << endTime << "} has " << logEvents.size()
           << " events: ";
        for(auto const &event: logEvents)
        {
            ss << "<" << std::get <0>(event.first) << ", " << std::get <1>(event.first) << ", "
               << std::get <2>(event.first) << ">: " << event.second.toString();
        }
//        LOG_DEBUG("string size in StoryChunk::toString(): {}", ss.str().size());
        return ss.str();
    }

    std::string chronicleName;
    StoryName storyName;
    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;

    std::map <EventSequence, LogEventHVL> logEvents;

};

struct OffsetMapEntry
{
    EventSequence eventId;
    EventOffsetSize offsetSize;

    OffsetMapEntry()
    {
        eventId = EventSequence(0, 0, 0);
        offsetSize = EventOffsetSize(0, 0);
    }

    OffsetMapEntry(EventSequence eventId, EventOffsetSize offsetSize): eventId(
            std::move(eventId)), offsetSize(std::move(offsetSize))
    {}
};

}
#endif
