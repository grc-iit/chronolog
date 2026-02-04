#ifndef CHRONOKVS_LOGGER_H
#define CHRONOKVS_LOGGER_H

#include <iostream>
#include <string>
#include <cstdint>
#include <sstream>

namespace chronokvs
{

/**
 * @enum LogLevel
 * @brief Defines logging severity levels for ChronoKVS
 *
 * Levels are ordered from most verbose (TRACE) to least verbose (OFF).
 * In Release builds (NDEBUG defined), TRACE and DEBUG are completely removed.
 */
enum class LogLevel : std::uint8_t
{
    TRACE = 0,    // Most verbose - function entry/exit, detailed flow
    DEBUG = 1,    // Debug information - parameters, intermediate states
    INFO = 2,     // Informational - significant operations
    WARNING = 3,  // Warning - potential issues
    ERROR = 4,    // Error - operation failures
    CRITICAL = 5, // Critical - severe failures
    OFF = 6       // No logging
};

/**
 * @brief Get the default log level based on build type
 * @return LogLevel::DEBUG in Debug builds, LogLevel::ERROR in Release builds
 */
inline LogLevel getDefaultLogLevel()
{
#ifdef NDEBUG
    return LogLevel::ERROR; // Release: errors only
#else
    return LogLevel::DEBUG; // Debug: show debug logs
#endif
}

/**
 * @brief Convert LogLevel to string representation
 * @param level The log level to convert
 * @return String representation of the level
 */
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

/**
 * @brief Internal logging function
 * @param level The log level
 * @param message The message to log
 */
inline void log_message(LogLevel level, const std::string& message)
{
    std::cerr << "[ChronoKVS][" << logLevelToString(level) << "] " << message << std::endl;
}

/**
 * @brief Variadic template helper for formatting log messages
 */
template <typename... Args>
inline std::string format_log_message(const char* fmt, Args&&... args)
{
    std::ostringstream oss;
    oss << fmt;
    // Simple implementation - just concatenate for now
    // This can be enhanced later with printf-style or fmt library
    ((oss << " " << std::forward<Args>(args)), ...);
    return oss.str();
}

/**
 * @brief Specialization for single string argument (most common case)
 */
inline std::string format_log_message(const char* msg) { return std::string(msg); }

/**
 * @brief Specialization for string with one argument
 */
template <typename T>
inline std::string format_log_message(const char* fmt, T&& arg)
{
    std::ostringstream oss;
    oss << fmt << arg;
    return oss.str();
}

} // namespace chronokvs

// ============================================================================
// LOGGING MACROS
// ============================================================================

/**
 * TRACE and DEBUG macros - only compiled in Debug builds
 * These are completely removed in Release builds (zero overhead)
 */
#ifdef NDEBUG
#define CHRONOKVS_TRACE(level, ...)
#define CHRONOKVS_DEBUG(level, ...)
#else
#define CHRONOKVS_TRACE(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::TRACE)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::TRACE, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_DEBUG(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::DEBUG)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::DEBUG, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)
#endif

/**
 * INFO, WARNING, ERROR, CRITICAL macros - always available
 * These work in both Debug and Release builds
 */
#define CHRONOKVS_INFO(level, ...)                                                                                     \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::INFO)                                                                       \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::INFO, chronokvs::format_log_message(__VA_ARGS__));             \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_WARNING(level, ...)                                                                                  \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::WARNING)                                                                    \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::WARNING, chronokvs::format_log_message(__VA_ARGS__));          \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_ERROR(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::ERROR)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::ERROR, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_CRITICAL(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::CRITICAL)                                                                   \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::CRITICAL, chronokvs::format_log_message(__VA_ARGS__));         \
        }                                                                                                              \
    } while(0)

#endif // CHRONOKVS_LOGGER_H
