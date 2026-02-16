#ifndef CHRONOLOG_CHRONO_MONITOR_H
#define CHRONOLOG_CHRONO_MONITOR_H

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>

#include "log_level.h"

namespace chronolog
{

namespace detail
{

template <typename T>
inline std::string to_str(const T& val)
{
    if constexpr(std::is_constructible_v<std::string, const T&>)
    {
        return std::string(val);
    }
    else if constexpr(std::is_arithmetic_v<std::decay_t<T>>)
    {
        return std::to_string(val);
    }
    else
    {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }
}

inline std::string format(const char* fmt_str) { return std::string(fmt_str); }

inline std::string format(const std::string& fmt_str) { return fmt_str; }

template <typename... Args>
inline std::string format(const char* fmt_str, const Args&... args)
{
    const std::string arg_strs[] = {to_str(args)...};
    std::string result;
    const char* p = fmt_str;
    std::size_t idx = 0;
    constexpr std::size_t n = sizeof...(Args);
    while(*p)
    {
        if(*p == '{' && *(p + 1) == '}' && idx < n)
        {
            result += arg_strs[idx++];
            p += 2;
        }
        else
        {
            result += *p++;
        }
    }
    return result;
}

} // namespace detail

/**
 * @class chrono_monitor
 * @brief Singleton logger with customizable configuration.
 *
 * Provides a unified logging interface. Wraps the underlying logging library
 * so that consumers of the public headers do not depend on it directly.
 */
class chrono_monitor
{
public:
    /**
     * @brief Initializes the logger with the specified configuration.
     *
     * @param logType      The type of logger ("file" or "console").
     * @param location     The file location if logType is "file".
     * @param logLevel     The logging level.
     * @param loggerName   The name of the logger, used in formatted output.
     * @param logFileSize  Maximum size of log file before rotating (in Bytes).
     * @param logFileNum   Number of log files to maintain before overwriting.
     * @param flushLevel   The logging level at which the logger flushes to file.
     *
     * @return 0 on success, 1 on error.
     */
    static int initialize(const std::string& logType,
                          const std::string& location,
                          LogLevel logLevel,
                          const std::string& loggerName,
                          std::size_t logFileSize = 104857600,
                          std::size_t logFileNum = 3,
                          LogLevel flushLevel = LogLevel::warn);

    static void trace(const std::string& msg);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);
    static void critical(const std::string& msg);

    chrono_monitor(const chrono_monitor&) = delete;
    chrono_monitor& operator=(const chrono_monitor&) = delete;
    ~chrono_monitor() = default;

private:
    chrono_monitor() = default;
};

/**
 * @def LOG_TRACE(...)
 * @brief Trace logging macro.
 * Logs a trace message when NDEBUG is not defined. Does nothing otherwise.
 */

/**
 * @def LOG_DEBUG(...)
 * @brief Debug logging macro.
 * Logs a debug message when NDEBUG is not defined. Does nothing otherwise.
 */

/**
 * @def LOG_INFO(...)
 * @brief Info logging macro.
 */

/**
 * @def LOG_WARNING(...)
 * @brief Warning logging macro.
 */

/**
 * @def LOG_ERROR(...)
 * @brief Error logging macro.
 */

/**
 * @def LOG_CRITICAL(...)
 * @brief Critical logging macro.
 */

#ifdef NDEBUG
#define LOG_TRACE(...)
#define LOG_DEBUG(...)
#else
#define LOG_TRACE(...) chronolog::chrono_monitor::trace(chronolog::detail::format(__VA_ARGS__))
#define LOG_DEBUG(...) chronolog::chrono_monitor::debug(chronolog::detail::format(__VA_ARGS__))
#endif
#define LOG_INFO(...) chronolog::chrono_monitor::info(chronolog::detail::format(__VA_ARGS__))
#define LOG_WARNING(...) chronolog::chrono_monitor::warn(chronolog::detail::format(__VA_ARGS__))
#define LOG_ERROR(...) chronolog::chrono_monitor::error(chronolog::detail::format(__VA_ARGS__))
#define LOG_CRITICAL(...) chronolog::chrono_monitor::critical(chronolog::detail::format(__VA_ARGS__))

} // namespace chronolog

#endif // CHRONOLOG_CHRONO_MONITOR_H
