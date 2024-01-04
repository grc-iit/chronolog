//
// Created by eneko on 12/1/23.
//

#include "log.h"
#include <spdlog/spdlog.h>

std::shared_ptr <spdlog::logger> Logger::logger = nullptr;

void Logger::initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                        , const std::string &loggerName)
{
    if(!logger)
    {
        if(logType == "file")
        {
            logger = spdlog::basic_logger_mt(loggerName, location);
        }
        else if(logType == "console")
        {
            logger = spdlog::stdout_color_mt(loggerName);
        }
        else
        {
            throw std::invalid_argument("Invalid log type");
        }

        logger->set_level(logLevel);
    }
    else
    {
        throw std::logic_error("Logger is already initialized");
    }
}

/*void Logger::changeConfiguration(const std::string &newLogType, const std::string &newLocation
                                 , spdlog::level::level_enum newLogLevel, const std::string &newLoggerName)
{
    if(logger)
    {
        // Check if any parameter is provided for change
        if(!newLogType.empty() || !newLocation.empty() || newLogLevel != spdlog::level::off || !newLoggerName.empty())
        {
            // Check if logger name is changing
            if(!newLoggerName.empty() && logger->name() != newLoggerName)
            {
                // Reset the existing logger and initialize with new name and configurations
                logger.reset();
                logger = nullptr;
                initialize(newLogType, newLocation, newLogLevel, newLoggerName);
            }
            else
            {
                // Modify existing logger configuration
                if(!newLogType.empty())
                {
                    // Handle changing log type (file/console)
                    // For simplicity, this example does not support changing log type
                    throw std::logic_error("Changing log type is not supported");
                }

                if(!newLocation.empty())
                {
                    // Handle changing file location (if applicable)
                    // For simplicity, this example does not support changing file location
                    throw std::logic_error("Changing file location is not supported");
                }

                if(newLogLevel != spdlog::level::off)
                {
                    // Set the new logging level
                    logger->set_level(newLogLevel);
                }
            }
        }
        else
        {
            throw std::invalid_argument("No new configuration provided");
        }
    }
    else
    {
        throw std::logic_error("Logger not initialized");
    }
}*/

void Logger::changeConfiguration(const std::string& newLogType, const std::string& newLocation, spdlog::level::level_enum newLogLevel, const std::string& newLoggerName)
{
    // Reset the existing logger instance and reinitialize with new configurations
    logger.reset();
    logger = nullptr;
    // Check if logger with the same name exists
    if (spdlog::get(newLoggerName))
    {
        // If logger with the same name exists, unregister it from the spdlog registry
        spdlog::drop(newLoggerName);
    }
    initialize(newLogType, newLocation, newLogLevel, newLoggerName);
}

std::shared_ptr <spdlog::logger> Logger::getLogger()
{
    if(!logger)
    {
        throw std::logic_error("Logger not initialized");
    }
    return logger;
}
