#ifndef CHRONOLOG_LOG_LEVEL_H
#define CHRONOLOG_LOG_LEVEL_H

namespace chronolog
{

enum class LogLevel
{
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    err = 4,
    critical = 5,
    off = 6
};

inline const char* to_string(LogLevel level)
{
    switch(level)
    {
        case LogLevel::trace:
            return "trace";
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warn:
            return "warning";
        case LogLevel::err:
            return "error";
        case LogLevel::critical:
            return "critical";
        case LogLevel::off:
            return "off";
        default:
            return "unknown";
    }
}

} // namespace chronolog

#endif // CHRONOLOG_LOG_LEVEL_H
