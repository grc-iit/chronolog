#ifndef CHRONOKVS_TYPES_H_
#define CHRONOKVS_TYPES_H_

#include <string>
#include <cstdint>
#include <utility>

namespace chronokvs
{

/**
 * @brief Represents a timestamped event in the ChronoKVS system
 * 
 * This structure holds both the timestamp and value of an event,
 * providing a type-safe way to handle event data across the codebase.
 */
struct EventData {
    std::uint64_t timestamp;  ///< The timestamp of the event
    std::string value;        ///< The value/content of the event

    /**
     * @brief Construct a new Event Data object
     * 
     * @param ts The timestamp of the event
     * @param val The value/content of the event
     */
    EventData(std::uint64_t ts, std::string val) 
        : timestamp(ts), value(std::move(val)) {}
        
    /**
     * @brief Construct from a pair (for compatibility with existing code)
     * 
     * @param pair A pair of timestamp and value
     */
    EventData(const std::pair<std::uint64_t, std::string>& pair)
        : timestamp(pair.first), value(pair.second) {}
};

} // namespace chronokvs

#endif // CHRONOKVS_TYPES_H_
