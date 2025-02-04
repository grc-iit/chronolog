#ifndef CHRONOLOG_TYPE_DEFINITIONS_H
#define CHRONOLOG_TYPE_DEFINITIONS_H


#include <string>
#include <ostream>
#include <cstring>
#include <tuple>
#include "H5Cpp.h"

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

class LogEventHVL
{
public:
    uint64_t storyId;
    uint64_t eventTime{};
    uint32_t clientId{};
    uint32_t eventIndex{};
    hvl_t logRecord{};

    LogEventHVL(): storyId(0), eventTime(0), clientId(0), eventIndex(0)
    {
        logRecord.len = 0;
        logRecord.p = nullptr;
    };

    LogEventHVL(uint64_t story_id, uint64_t event_time, uint32_t client_id, uint32_t event_index, hvl_t log_record)
            : storyId(story_id), eventTime(event_time), clientId(client_id), eventIndex(event_index)
    {
        logRecord.len = log_record.len;
        logRecord.p = new uint8_t[log_record.len];
        std::memcpy(logRecord.p, log_record.p, log_record.len);
    };

    ~LogEventHVL() {
        if (logRecord.p) {
            delete[] static_cast<uint8_t*>(logRecord.p);
            logRecord.p = nullptr;
        }
    }

    LogEventHVL(const LogEventHVL& other)
            : storyId(other.storyId), eventTime(other.eventTime), clientId(other.clientId), eventIndex(other.eventIndex) {
        logRecord.len = other.logRecord.len;
        logRecord.p = new uint8_t[logRecord.len];
        std::memcpy(logRecord.p, other.logRecord.p, logRecord.len);
    }

    [[nodiscard]] uint64_t const &time() const
    { return eventTime; }

    [[nodiscard]] uint32_t const &index() const
    { return eventIndex; }

    [[nodiscard]] std::string toString() const
    {
        std::string str =
                "storyId: " + std::to_string(storyId) + "\n" + "eventTime: " + std::to_string(eventTime) + "\n" +
                "clientId: " + std::to_string(clientId) + "\n" + "eventIndex: " + std::to_string(eventIndex) + "\n";
        str += "logRecord: ";
        char*ptr = static_cast<char*>(logRecord.p);
        for(hsize_t i = 0; i < logRecord.len; i++)
        {
            str += *(ptr + i);
            str += " ";
        }
        str += "\n";
//        LOG_DEBUG("logRecord len: {}", logRecord.len);
        return str;
    }

    bool operator==(const LogEventHVL &other) const
    {
        return storyId == other.storyId && eventTime == other.eventTime && clientId == other.clientId &&
               eventIndex == other.eventIndex;
    }

    bool operator!=(const LogEventHVL &other) const
    {
        return !(*this == other);
    }

    bool operator<(const LogEventHVL &other) const
    {
        if (eventTime < other.eventTime) {
            return true;
        } else if (eventTime == other.eventTime) {
            if (clientId < other.clientId) {
                return true;
            } else if (clientId == other.clientId) {
                return eventIndex < other.eventIndex;
            }
        }
        return false;
    }

    LogEventHVL& operator=(const LogEventHVL& other) {
        if (this != &other) {
            storyId = other.storyId;
            eventTime = other.eventTime;
            clientId = other.clientId;
            eventIndex = other.eventIndex;

            if (logRecord.p) {
                delete[] static_cast<uint8_t*>(logRecord.p);
            }

            logRecord.len = other.logRecord.len;
            logRecord.p = new uint8_t[logRecord.len];
            std::memcpy(logRecord.p, other.logRecord.p, logRecord.len);
        }
        return *this;
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
