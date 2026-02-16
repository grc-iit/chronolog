#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono_monitor.h>

namespace chronolog
{

static std::shared_ptr<spdlog::logger> g_logger = nullptr;
static std::mutex g_mutex;

static spdlog::level::level_enum to_spdlog_level(LogLevel level)
{
    return static_cast<spdlog::level::level_enum>(static_cast<int>(level));
}

int chrono_monitor::initialize(const std::string& logType, const std::string& location, LogLevel logLevel,
                               const std::string& loggerName, std::size_t logFileSize, std::size_t logFileNum,
                               LogLevel flushLevel)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_logger)
    {
        return 0;
    }
    try
    {
        std::shared_ptr<spdlog::sinks::sink> sink = nullptr;
        if(logType == "file")
        {
            sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(location, logFileSize, logFileNum);
        }
        else if(logType == "console")
        {
            sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        }
        else
        {
            std::cerr << "[Logger] Invalid log type" << std::endl;
            return 1;
        }
        g_logger = std::make_shared<spdlog::logger>(loggerName, sink);
        g_logger->flush_on(to_spdlog_level(flushLevel));
        g_logger->set_level(to_spdlog_level(logLevel));
    }
    catch(const spdlog::spdlog_ex& ex)
    {
        std::cerr << "[Logger] Logger initialization failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}

void chrono_monitor::trace(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->trace(msg);
    }
}

void chrono_monitor::debug(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->debug(msg);
    }
}

void chrono_monitor::info(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->info(msg);
    }
}

void chrono_monitor::warn(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->warn(msg);
    }
}

void chrono_monitor::error(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->error(msg);
    }
}

void chrono_monitor::critical(const std::string& msg)
{
    if(g_logger)
    {
        g_logger->critical(msg);
    }
}

} // namespace chronolog
