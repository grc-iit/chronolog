//
// Created by kfeng on 4/5/22.
//

#ifndef CHRONOLOG_LOG_H
#define CHRONOLOG_LOG_H

#include <spdlog/spdlog.h>
#include <mutex>


/**
 * @def LOGT(...)
 * @brief Trace logging macro.
 * Logs a trace message when NDEBUG is not defined. Does nothing otherwise.
 */

/**
 * @def LOGD(...)
 * @brief Debug logging macro.
 * Logs a debug message when NDEBUG is not defined. Does nothing otherwise.
 */

/**
 * @def LOGI(...)
 * @brief Info logging macro.
 * Logs an info message regardless of NDEBUG definition.
 */

/**
 * @def LOGW(...)
 * @brief Warning logging macro.
 * Logs a warning message regardless of NDEBUG definition.
 */

/**
 * @def LOGE(...)
 * @brief Error logging macro.
 * Logs an error message regardless of NDEBUG definition.
 */

/**
 * @def LOGC(...)
 * @brief Critical logging macro.
 * Logs a critical message regardless of NDEBUG definition.
 */

// Custom logging macros based on build type
#ifdef NDEBUG
#define LOGT(...)
#define LOGD(...)
#else
#define LOGT(...) Logger::getInstance().trace(__VA_ARGS__)
#define LOGD(...) Logger::getInstance().debug(__VA_ARGS__)
#endif
#define LOGI(...) Logger::getInstance().info(__VA_ARGS__)
#define LOGW(...) Logger::getInstance().warn(__VA_ARGS__)
#define LOGE(...) Logger::getInstance().error(__VA_ARGS__)
#define LOGC(...) Logger::getInstance().critical(__VA_ARGS__)


/**
 * @class Logger
 * @brief The Logger class provides a singleton logger with customizable configuration.
 *
 * This class implements a singleton pattern to provide a unified logging interface
 * throughout the application. It allows configuration of log type, location, level,
 * and name. It is designed to work with the spdlog library.
 */
class Logger
{
public:

    /**
     * @brief Initializes the logger with the specified configuration.
     *
     * This static function sets up the logger based on given parameters. It should be
     * called before any logging operations. The function handles logger type,
     * location, level, name, file size for rotation, and number of files to keep.
     *
     * @param logType      The type of logger ("file" or "console").
     * @param location     The file location if logType is "file".
     * @param logLevel     The logging level for the logger (e.g., debug, info, error).
     * @param loggerName   The name of the logger, used in formatted output.
     * @param logFileSize  Maximum size of log file before rotating (in Bytes).
     * @param logFileNum   Number of log files to maintain before overwriting.
     *
     * @return             Returns 0 if the logger was initialized successfully,
     *                     and returns 1 if there was an error during initialization.
     */
    static int initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                          , const std::string &loggerName, const std::size_t &logFileSize
                          , const std::size_t &logFileNum);


    /**
     * @brief Accessor for the logger instance.
     *
     * Provides access to the singleton logger instance. The function returns
     * a reference to the internal spdlog::logger instance.
     *
     * @return Reference to spdlog::logger instance.
     */
    static spdlog::logger &getInstance();

    // Delete copy constructor and assignment operator
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;
    ~Logger() = default;

private:
    //Private Constructor
    Logger() = default;

    /**
     * @brief The shared pointer to the spdlog logger instance.
     *
     * This static member holds the instance of the spdlog logger used by the Logger class.
     */
    static std::shared_ptr <spdlog::logger> logger;

    /**
     * @brief Mutex for thread safety.
     *
     * This mutex is used to ensure thread-safe initialization and access to the logger instance.
     */
    static std::mutex mutex;
};

#endif //CHRONOLOG_LOG_H
