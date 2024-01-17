//
// Created by kfeng on 4/5/22.
//

#ifndef CHRONOLOG_LOG_H
#define CHRONOLOG_LOG_H

#include <spdlog/spdlog.h>
#include <mutex>

// Custom logging macros based on build type
#ifdef NDEBUG
#define LOGT(...)
#define LOGD(...)
#else
#define LOGT(...) Logger::getLogger()->trace(__VA_ARGS__)
#define LOGD(...) Logger::getLogger()->debug(__VA_ARGS__)
#endif
#define LOGI(...) Logger::getLogger()->info(__VA_ARGS__)
#define LOGW(...) Logger::getLogger()->warn(__VA_ARGS__)
#define LOGE(...) Logger::getLogger()->error(__VA_ARGS__)
#define LOGC(...) Logger::getLogger()->critical(__VA_ARGS__)


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
     * This static function configures and initializes the logger. It must be called
     * before any logging activities. Returns 0 on success and 1 on failure.
     *
     * @param logType      The type of logger ("file" or "console").
     * @param location     The file location if logType is "file".
     * @param logLevel     The logging level for the logger (e.g., debug, info, error).
     * @param loggerName   The name of the logger, used in formatted output.
     *
     * @return             Returns 0 if the logger was initialized successfully,
     *                     and returns 1 if there was an error during initialization.
     */
    static int initialize(const std::string &logType, const std::string &location, spdlog::level::level_enum logLevel
                          , const std::string &loggerName);


    /**
     * @brief Gets the shared pointer to the logger.
     *
     * This static function provides access to the logger instance. It returns a
     * shared pointer to the spdlog logger.
     *
     * @return A shared pointer to the spdlog::logger instance.
     */
    static std::shared_ptr <spdlog::logger> getLogger();

    // Delete copy constructor and copy assignment operator to enforce singleton pattern
    Logger(const Logger &) = delete;

    Logger &operator=(const Logger &) = delete;

private:
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

    Logger() = default;

    ~Logger() = default;
};

#endif //CHRONOLOG_LOG_H
