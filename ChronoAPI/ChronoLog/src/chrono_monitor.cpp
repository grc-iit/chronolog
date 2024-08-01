//
// Created by eneko on 12/1/23.
//

#include "chrono_monitor.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <iostream>
#include <filesystem>

namespace chronolog
{

std::shared_ptr <spdlog::logger> chrono_monitor::logger = nullptr;
std::mutex chrono_monitor::mutex;

int chrono_monitor::initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                               , const std::string &loggerName, const std::size_t &logFileSize, const std::size_t &logFileNum
                               , spdlog::level::level_enum flushLevel)
{
    std::lock_guard <std::mutex> lock(mutex);
    if(logger)
    {
        return 0;
    }
    try
    {
        std::shared_ptr <spdlog::sinks::sink> sink = nullptr;
        if(logType == "file")
        {
            /*
             * There is an alternative for rotating daily:
             * Link: https://github.com/gabime/spdlog/blob/v1.x/include/spdlog/sinks/daily_file_sink.h
             * They use C++ standard library clock:
             * Link: https://github.com/gabime/spdlog/blob/696db97f672e9082e50e50af315d0f4234c82397/include/spdlog/common.h#L141
             */
            sink = std::make_shared <spdlog::sinks::rotating_file_sink_mt>(location, logFileSize, logFileNum);
        }
        else if(logType == "console")
        {
            sink = std::make_shared <spdlog::sinks::stdout_color_sink_mt>();
        }
        else
        {
            std::cerr << "[Logger] Invalid log type" << std::endl;
            return 1;
        }
        logger = std::make_shared <spdlog::logger>(loggerName, sink);
        logger->flush_on(flushLevel);
        logger->set_level(logLevel);
    }
    catch(const spdlog::spdlog_ex &ex)
    {
        std::cerr << "[Logger] Logger initialization failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0; // already initialized
}

spdlog::logger &chrono_monitor::getInstance()
{
    if(logger)
    {
        return *logger;
    }
    else
    {
        std::cerr << "[Logger] Logger not initialized. Ending the program..." << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

} // namespace chronolog