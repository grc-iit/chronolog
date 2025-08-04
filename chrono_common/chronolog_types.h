#ifndef CHRONOLOG_TYPE_DEFINITIONS_H
#define CHRONOLOG_TYPE_DEFINITIONS_H


#include <string>
#include <ostream>
#include <cstring>
#include <tuple>

namespace chronolog
{

typedef std::string StoryName;
typedef std::string ChronicleName;
typedef uint64_t StoryId;
typedef uint64_t ChronicleId;
typedef uint64_t ClientId;

typedef uint64_t chrono_time;
typedef uint32_t chrono_index;

class LogEvent
{
public:
    LogEvent() = default;

    LogEvent(StoryId const &story_id, chrono_time event_time, ClientId client_id, chrono_index index
             , std::string const &record): storyId(story_id), eventTime(event_time), clientId(client_id), eventIndex(
            index), logRecord(record)
    {}

    StoryId storyId;
    uint64_t eventTime;
    ClientId clientId;
    uint32_t eventIndex;
    std::string logRecord; //INNA: replace with size_t  length; & void * data; later on

    StoryId const &getStoryId() const
    { return storyId; }

    uint64_t time() const
    { return eventTime; }

    ClientId const &getClientId() const
    { return clientId; }

    uint32_t index() const
    { return eventIndex; }

    std::string const &getRecord() const
    { return logRecord; }

    // serialization function used by thallium RPC providers
    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT(storyId, eventTime, clientId, eventIndex, logRecord);
    }

    bool operator==(const LogEvent &other) const
    {
        return (storyId == other.storyId && eventTime == other.eventTime && clientId == other.clientId &&
                eventIndex == other.eventIndex );
    }

    // convert to string
    [[nodiscard]] std::string toString() const
    {
        std::string str = "StoryId: " + std::to_string(storyId) + " EventTime: " + std::to_string(eventTime) +
                          " ClientId: " + std::to_string(clientId) + " EventIndex: " + std::to_string(eventIndex) +
                          " LogRecord: " + logRecord;
        return str;
    }
};
}

inline std::ostream &operator<<(std::ostream &out, chronolog::LogEvent const &event)
{
    out << "event : " << event.getStoryId() << ":" << event.time() << ":" << event.getClientId() << ":" << event.index()
        << ":" << event.getRecord();
    return out;
}

#endif
