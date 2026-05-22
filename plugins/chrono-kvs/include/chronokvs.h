#ifndef CHRONOKVS_H_
#define CHRONOKVS_H_

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>

#include "chronokvs_types.h"
#include "chronokvs_logger.h"

namespace chronokvs
{

// Forward declaration of the private mapper type (header lives in src/)
class ChronoKVSMapper;

class ChronoKVS
{
private:
    std::unique_ptr<ChronoKVSMapper> mapper;
    LogLevel logLevel_;

    // Private constructors. May throw on configuration or connection failure;
    // the public Create() factories catch those exceptions at the library
    // boundary and signal failure via a nullptr return.
    explicit ChronoKVS(LogLevel level);
    explicit ChronoKVS(const std::string& config_path, LogLevel level);

public:
    /**
     * @brief Create a ChronoKVS instance using built-in default ChronoLog
     *        client configuration (localhost deployment).
     *
     * This factory does not propagate exceptions across the library boundary:
     * any failure during configuration loading or ChronoLog connection is
     * logged at the configured @p level and signalled by returning nullptr.
     *
     * @param level
     *     The logging level to use. Default is DEBUG in debug builds, ERROR in release builds.
     *
     * @return std::unique_ptr<ChronoKVS>
     *     A connected ChronoKVS instance, or nullptr if construction failed.
     */
    static std::unique_ptr<ChronoKVS> Create(LogLevel level = getDefaultLogLevel()) noexcept;

    /**
     * @brief Create a ChronoKVS instance using a ChronoLog client configuration file.
     *
     * Loads the JSON configuration at @p config_path and uses the resulting portal,
     * query and logging settings to connect to ChronoLog. Pass an empty string to
     * fall back to the built-in defaults (localhost deployment).
     *
     * This factory does not propagate exceptions across the library boundary:
     * any failure during configuration loading or ChronoLog connection is
     * logged at the configured @p level and signalled by returning nullptr.
     *
     * @param config_path
     *     Path to a ChronoLog client configuration JSON file. Empty means
     *     "use defaults".
     * @param level
     *     The logging level to use. Default is DEBUG in debug builds, ERROR in release builds.
     *
     * @return std::unique_ptr<ChronoKVS>
     *     A connected ChronoKVS instance, or nullptr if @p config_path could
     *     not be loaded or the ChronoLog connection failed.
     */
    static std::unique_ptr<ChronoKVS> Create(const std::string& config_path,
                                             LogLevel level = getDefaultLogLevel()) noexcept;

    /**
     * @brief Get the current log level
     * @return The current LogLevel
     */
    LogLevel getLogLevel() const { return logLevel_; }

    ~ChronoKVS();

    std::uint64_t put(const std::string& key, const std::string& value);

    std::string get(const std::string& key, uint64_t timestamp);

    std::vector<EventData> get_history(const std::string& key);

    /**
     * @brief Retrieve all events for the given key within a time range.
     *
     * Returns all events associated with @p key whose timestamps fall within the
     * half-open interval [@p start_timestamp, @p end_timestamp), i.e. events
     * with timestamp >= @p start_timestamp and < @p end_timestamp.
     *
     * @param key
     *     The key whose events should be retrieved.
     * @param start_timestamp
     *     The inclusive lower bound of the timestamp range.
     * @param end_timestamp
     *     The exclusive upper bound of the timestamp range.
     *
     * @return std::vector<EventData>
     *     A vector of events for the specified key in the given time range.
     *     The vector may be empty if no events fall within the range.
     */
    std::vector<EventData> get_range(const std::string& key, uint64_t start_timestamp, uint64_t end_timestamp);

    /**
     * @brief Retrieve the earliest event for the given key.
     *
     * Looks up the earliest (oldest) event associated with @p key.
     *
     * @param key
     *     The key whose earliest event should be retrieved.
     *
     * @return std::optional<EventData>
     *     The earliest event for the specified key, or std::nullopt if no
     *     events exist for that key.
     */
    std::optional<EventData> get_earliest(const std::string& key);

    /**
     * @brief Retrieve the latest event for the given key.
     *
     * Looks up the latest (most recent) event associated with @p key.
     *
     * @param key
     *     The key whose latest event should be retrieved.
     *
     * @return std::optional<EventData>
     *     The latest event for the specified key, or std::nullopt if no
     *     events exist for that key.
     */
    std::optional<EventData> get_latest(const std::string& key);

    /**
     * @brief Flush all cached story handles to commit pending writes.
     *
     * ChronoKVS caches story handles to improve write performance by avoiding
     * repeated acquire/release cycles. Call this method to release all cached
     * handles and ensure data is committed for propagation.
     *
     * This should be called before waiting for data to be available for read
     * operations, or when you need to ensure all writes are committed.
     */
    void flush();
};

} // namespace chronokvs

#endif // CHRONOKVS_H_
