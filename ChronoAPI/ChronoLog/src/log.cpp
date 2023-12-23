//
// Created by eneko on 12/1/23.
//

#include "log.h"

/**
 * @brief The shared pointer to the logger.
 *
 * This static member holds the shared pointer to the logger instance.
 * It is initialized to nullptr and gets set when the logger is initialized
 * using the Logger::initialize method.
 */
std::shared_ptr <spdlog::logger> Logger::logger = nullptr;

/**
 * @brief Initializes the logger with the specified configuration.
 * @param logType The type of logger ("file" or "console").
 * @param location The file location if logType is "file".
 * @param logLevel The logging level for the logger.
 * @param loggerName The name of the logger.
 */
void Logger::initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel, const std::string &loggerName)
{
    if (!logger)
    {
        if (logType == "file")
        {
            // Create a file logger
            logger = spdlog::basic_logger_mt(loggerName, location);
        }
        else if (logType == "console")
        {
            // Create a console logger
            logger = spdlog::stdout_color_mt(loggerName);
        }
        else
        {
            // Invalid log type, you may want to handle this error condition appropriately
            throw std::invalid_argument("Invalid log type");
        }

        // Set the logging level
        logger->set_level(logLevel);
    }
    else
    {
        // Logger is already initialized
        throw std::logic_error("Logger is already initialized");
    }
}


/**
 * @brief Gets the shared pointer to the logger.
 * @return A shared pointer to the logger.
 */
std::shared_ptr <spdlog::logger> Logger::getLogger()
{
    if(!logger)
    {
        // Logger not initialized, you may want to handle this error condition appropriately
        throw std::logic_error("Logger not initialized");
    }
    return logger;
}