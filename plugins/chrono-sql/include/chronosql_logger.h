#ifndef CHRONOSQL_LOGGER_H
#define CHRONOSQL_LOGGER_H

#include <iostream>
#include <string>
#include <cstdint>
#include <sstream>

namespace chronosql
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
    std::cerr << "[ChronoSQL][" << logLevelToString(level) << "] " << message << std::endl;
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

} // namespace chronosql

#ifdef NDEBUG
#define CHRONOSQL_TRACE(level, ...)
#define CHRONOSQL_DEBUG(level, ...)
#else
#define CHRONOSQL_TRACE(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::TRACE)                                                                      \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::TRACE, chronosql::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOSQL_DEBUG(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::DEBUG)                                                                      \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::DEBUG, chronosql::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)
#endif

#define CHRONOSQL_INFO(level, ...)                                                                                     \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::INFO)                                                                       \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::INFO, chronosql::format_log_message(__VA_ARGS__));             \
        }                                                                                                              \
    } while(0)

#define CHRONOSQL_WARNING(level, ...)                                                                                  \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::WARNING)                                                                    \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::WARNING, chronosql::format_log_message(__VA_ARGS__));          \
        }                                                                                                              \
    } while(0)

#define CHRONOSQL_ERROR(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::ERROR)                                                                      \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::ERROR, chronosql::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOSQL_CRITICAL(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronosql::LogLevel::CRITICAL)                                                                   \
        {                                                                                                              \
            chronosql::log_message(chronosql::LogLevel::CRITICAL, chronosql::format_log_message(__VA_ARGS__));         \
        }                                                                                                              \
    } while(0)

#endif // CHRONOSQL_LOGGER_H
