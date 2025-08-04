// File: include/chronolog/LogEventHVL.h
#ifndef CHRONOLOG_LOGEVENT_HVL_H
#define CHRONOLOG_LOGEVENT_HVL_H

#include <string>
#include <cstring>
#include <H5Cpp.h>


namespace chronolog {

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

} // namespace chronolog

#endif // CHRONOLOG_LOGEVENT_HVL_H
