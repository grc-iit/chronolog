//
// Created by eneko on 12/1/23.
//

#include "log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <iostream>

std::shared_ptr <spdlog::logger> Logger::logger = nullptr;
std::mutex Logger::mutex;

int Logger::initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                       , const std::string &loggerName)
{
    std::lock_guard <std::mutex> lock(mutex);
    if(logger)
    {
        Logger::getLogger()->debug("[Logger] Logger is already initialized");
        return 0;
    }
    if(logType == "file")
    {
        try
        {
            // Create a rotating_file_sink with a max file size of 5MB and max 3 files
            auto rotating_sink = std::make_shared <spdlog::sinks::rotating_file_sink_mt>(location, 1024 * 1024 * 5, 3);
            logger = std::make_shared <spdlog::logger>(loggerName, rotating_sink);
        }
        catch(const spdlog::spdlog_ex &ex)
        {
            std::cerr << "[Logger] Log initialization failed: " << ex.what() << std::endl;
            return 1;
        }

    }
    else if(logType == "console")
    {
        try
        {
            logger = spdlog::stdout_color_mt(loggerName);
        }
        catch(const spdlog::spdlog_ex &ex)
        {
            std::cerr << "[Logger] Logger initialization failed: " << ex.what() << std::endl;
            return 1;
        }

    }
    else
    {
        std::cerr << "[Logger] Invalid log type " << std::endl;
        return 1;
    }
    // Adjust log level based on build type
#ifdef NDEBUG  // Release build
    if(logLevel == spdlog::level::trace || logLevel == spdlog::level::debug)
    {
        logger->set_level(spdlog::level::info);  // Set to info level for release builds if trace/debug is requested
    }
    else
    {
        logger->set_level(logLevel);  // Set the provided log level for release builds
    }
#else  // Debug build
    logger->set_level(logLevel);  // Set the provided log level for debug builds
#endif
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
