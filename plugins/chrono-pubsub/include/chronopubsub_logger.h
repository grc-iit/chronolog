#ifndef CHRONOPUBSUB_LOGGER_H
#define CHRONOPUBSUB_LOGGER_H

#include <iostream>
#include <string>
#include <cstdint>
#include <sstream>

namespace chronopubsub
{

enum class LogLevel : std::uint8_t
{
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    ERROR = 4,
    CRITICAL = 5,
    OFF = 6
};

inline LogLevel getDefaultLogLevel()
{
#ifdef NDEBUG
    return LogLevel::ERROR;
#else
    return LogLevel::DEBUG;
#endif
}

inline const char* logLevelToString(LogLevel level)
{
    switch(level)
    {
        case LogLevel::TRACE:
            return "TRACE";
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARNING:
            return "WARNING";
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::CRITICAL:
            return "CRITICAL";
        case LogLevel::OFF:
            return "OFF";
        default:
            return "UNKNOWN";
    }
}

inline void log_message(LogLevel level, const std::string& message)
{
    std::cerr << "[ChronoPubSub][" << logLevelToString(level) << "] " << message << std::endl;
}

template <typename... Args>
inline std::string format_log_message(const char* fmt, Args&&... args)
{
    std::ostringstream oss;
    oss << fmt;
    ((oss << " " << std::forward<Args>(args)), ...);
    return oss.str();
}

inline std::string format_log_message(const char* msg) { return std::string(msg); }

template <typename T>
inline std::string format_log_message(const char* fmt, T&& arg)
{
    std::ostringstream oss;
    oss << fmt << arg;
    return oss.str();
}

} // namespace chronopubsub

#ifdef NDEBUG
#define CHRONOPUBSUB_TRACE(level, ...)
#define CHRONOPUBSUB_DEBUG(level, ...)
#else
#define CHRONOPUBSUB_TRACE(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::TRACE)                                                                   \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::TRACE, chronopubsub::format_log_message(__VA_ARGS__));   \
        }                                                                                                              \
    } while(0)

#define CHRONOPUBSUB_DEBUG(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::DEBUG)                                                                   \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::DEBUG, chronopubsub::format_log_message(__VA_ARGS__));   \
        }                                                                                                              \
    } while(0)
#endif

#define CHRONOPUBSUB_INFO(level, ...)                                                                                  \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::INFO)                                                                    \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::INFO, chronopubsub::format_log_message(__VA_ARGS__));    \
        }                                                                                                              \
    } while(0)

#define CHRONOPUBSUB_WARNING(level, ...)                                                                               \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::WARNING)                                                                 \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::WARNING, chronopubsub::format_log_message(__VA_ARGS__)); \
        }                                                                                                              \
    } while(0)

#define CHRONOPUBSUB_ERROR(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::ERROR)                                                                   \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::ERROR, chronopubsub::format_log_message(__VA_ARGS__));   \
        }                                                                                                              \
    } while(0)

#define CHRONOPUBSUB_CRITICAL(level, ...)                                                                              \
    do {                                                                                                               \
        if((level) <= chronopubsub::LogLevel::CRITICAL)                                                                \
        {                                                                                                              \
            chronopubsub::log_message(chronopubsub::LogLevel::CRITICAL,                                                \
                                      chronopubsub::format_log_message(__VA_ARGS__));                                  \
        }                                                                                                              \
    } while(0)

#endif // CHRONOPUBSUB_LOGGER_H
