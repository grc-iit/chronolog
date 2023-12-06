//
// Created by kfeng on 4/5/22.
//

#ifndef CHRONOLOG_LOG_H
#define CHRONOLOG_LOG_H

//#pragma once

#include <spdlog/spdlog.h>
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <memory>
#include <cstdio>
#include <cstring>

/**
 * @brief The Logger class provides a singleton logger with customizable configuration.
 */
class Logger {
public:
    /**
     * @brief Initializes the logger with the specified configuration.
     * @param logType The type of logger ("file" or "console").
     * @param location The file location if logType is "file".
     * @param logLevel The logging level for the logger.
     */
    static void initialize(const std::string& logType, const std::string& location, spdlog::level::level_enum logLevel);

    /**
     * @brief Gets the shared pointer to the logger.
     * @return A shared pointer to the logger.
     */
    static std::shared_ptr<spdlog::logger> getLogger();

private:
    /**
     * @brief Private constructor to enforce the singleton pattern.
     */
    Logger();

    /**
     * @brief Private destructor to enforce the singleton pattern.
     */
    ~Logger();

    /**
     * @brief The shared pointer to the logger.
     */
    static std::shared_ptr<spdlog::logger> logger;
};

#endif //CHRONOLOG_LOG_H