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
    LvlTrace = 0,    // Most verbose - function entry/exit, detailed flow
    LvlDebug = 1,    // Debug information - parameters, intermediate states
    LvlInfo = 2,     // Informational - significant operations
    LvlWarning = 3,  // Warning - potential issues
    LvlError = 4,    // Error - operation failures
    LvlCritical = 5, // Critical - severe failures
    LvlOff = 6       // No logging
};

/**
 * @brief Get the default log level based on build type
 * @return LogLevel::LvlDebug in Debug builds, LogLevel::LvlError in Release builds
 */
inline LogLevel getDefaultLogLevel()
{
#ifdef NDEBUG
    return LogLevel::LvlError; // Release: errors only
#else
    return LogLevel::LvlDebug; // Debug: show debug logs
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
        case LogLevel::LvlTrace:
            return "TRACE";
        case LogLevel::LvlDebug:
            return "DEBUG";
        case LogLevel::LvlInfo:
            return "INFO";
        case LogLevel::LvlWarning:
            return "WARNING";
        case LogLevel::LvlError:
            return "ERROR";
        case LogLevel::LvlCritical:
            return "CRITICAL";
        case LogLevel::LvlOff:
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
        if((level) <= chronokvs::LogLevel::LvlTrace)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlTrace, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_DEBUG(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::LvlDebug)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlDebug, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)
#endif

/**
 * INFO, WARNING, ERROR, CRITICAL macros - always available
 * These work in both Debug and Release builds
 */
#define CHRONOKVS_INFO(level, ...)                                                                                     \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::LvlInfo)                                                                       \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlInfo, chronokvs::format_log_message(__VA_ARGS__));             \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_WARNING(level, ...)                                                                                  \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::LvlWarning)                                                                    \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlWarning, chronokvs::format_log_message(__VA_ARGS__));          \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_ERROR(level, ...)                                                                                    \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::LvlError)                                                                      \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlError, chronokvs::format_log_message(__VA_ARGS__));            \
        }                                                                                                              \
    } while(0)

#define CHRONOKVS_CRITICAL(level, ...)                                                                                 \
    do {                                                                                                               \
        if((level) <= chronokvs::LogLevel::LvlCritical)                                                                   \
        {                                                                                                              \
            chronokvs::log_message(chronokvs::LogLevel::LvlCritical, chronokvs::format_log_message(__VA_ARGS__));         \
        }                                                                                                              \
    } while(0)

#endif // CHRONOKVS_LOGGER_H
