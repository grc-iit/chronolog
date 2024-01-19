//
// Created by eneko on 12/1/23.
//

#include "log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <iostream>
#include <filesystem>

std::shared_ptr <spdlog::logger> Logger::logger = nullptr;
std::mutex Logger::mutex;

int Logger::initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                       , const std::string &loggerName, const std::size_t &logFileSize, const std::size_t &logFileNum)
{
    std::lock_guard <std::mutex> lock(mutex);
    if(logger)
    {
        LOGD("[Logger] Logger is already initialized");
        return 0;
    }

    try
    {
        if(logType == "file")
        {
            auto rotating_sink = std::make_shared <spdlog::sinks::rotating_file_sink_mt>(location, logFileSize, logFileNum);
            logger = std::make_shared <spdlog::logger>(loggerName, rotating_sink);
        }
        else if(logType == "console")
        {
            logger = spdlog::stdout_color_mt(loggerName);
        }
        else
        {
            std::cerr << "[Logger] Invalid log type" << std::endl;
            return 1;
        }
        logger->set_level(logLevel);
    }
    catch(const spdlog::spdlog_ex &ex)
    {
        std::cerr << "[Logger] Logger initialization failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}

std::shared_ptr <spdlog::logger> Logger::getLogger()
{
    std::lock_guard <std::mutex> lock(mutex);
    if(logger)
    {
        return logger;
    }
    std::cerr
            << "[Logger] No logger is not initialized or you are trying to log before initializing the logger. Ending the program..."
            << std::endl;
    std::exit(EXIT_FAILURE);
}
