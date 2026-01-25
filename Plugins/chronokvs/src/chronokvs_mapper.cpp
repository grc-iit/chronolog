#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "chronokvs_mapper.h"
#include <chrono_monitor.h>

namespace chronokvs
{

// Constants for time range boundaries (used across multiple retrieval methods)
constexpr uint64_t MIN_TIMESTAMP = 1;                   // Earliest possible timestamp
constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000; // ~May 18, 2033 03:33:20 UTC

ChronoKVSMapper::ChronoKVSMapper() { chronoClientAdapter = std::make_unique<ChronoKVSClientAdapter>(); }

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string& key, const std::string& value)
{
    // Input validation
    if(key.empty())
    {
        LOG_ERROR("[ChronoKVS] Invalid input: key cannot be empty");
        throw std::invalid_argument("Key cannot be empty");
    }
    if(value.empty())
    {
        LOG_ERROR("[ChronoKVS] Invalid input: value cannot be empty for key='{}'", key);
        throw std::invalid_argument("Value cannot be empty");
    }

    LOG_DEBUG("[ChronoKVS] Storing key-value pair: key='{}', value_size={}", key, value.size());
    return chronoClientAdapter->storeEvent(key, value);
}

std::string ChronoKVSMapper::retrieveByKeyAndTs(const std::string& key, std::uint64_t timestamp)
{
    LOG_DEBUG("[ChronoKVS] Retrieving value for key='{}' at timestamp={}", key, timestamp);

    // Retrieve events in the narrow time range [timestamp, timestamp + 1)
    auto events = chronoClientAdapter->retrieveEvents(key, timestamp, timestamp + 1);

    if(events.empty())
    {
        LOG_DEBUG("[ChronoKVS] No events found for key='{}' at timestamp={}", key, timestamp);
        return "";
    }

    // Return the first event's value only if its timestamp matches the requested timestamp
    if(events[0].timestamp == timestamp)
    {
        LOG_DEBUG("[ChronoKVS] Found event for key='{}' at timestamp={}", key, timestamp);
        return events[0].value;
    }
    LOG_DEBUG("[ChronoKVS] No event found for key='{}' at exact timestamp={}", key, timestamp);
    return "";
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string& key)
{
    LOG_DEBUG("[ChronoKVS] Retrieving all events for key='{}'", key);

    // Retrieve all events for the given key using the full time range.
    // This ensures we capture all events regardless of their timestamp,
    // avoiding any potential data loss from timing uncertainties.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
    LOG_DEBUG("[ChronoKVS] Retrieved {} events for key='{}'", events.size(), key);
    return events;
}

std::vector<EventData> ChronoKVSMapper::retrieveByKeyAndRange(const std::string& key,
                                                              std::uint64_t start_timestamp,
                                                              std::uint64_t end_timestamp)
{
    // Validate timestamp range
    if(start_timestamp >= end_timestamp)
    {
        LOG_ERROR("[ChronoKVS] Invalid timestamp range for key='{}': start_timestamp={} >= end_timestamp={}",
                  key, start_timestamp, end_timestamp);
        throw std::invalid_argument("Invalid timestamp range: start_timestamp must be less than end_timestamp");
    }

    LOG_DEBUG("[ChronoKVS] Retrieving events for key='{}' range=[{}, {})", key, start_timestamp, end_timestamp);

    // Retrieve events for the given key within the specified time range [start_timestamp, end_timestamp).
    auto events = chronoClientAdapter->retrieveEvents(key, start_timestamp, end_timestamp);
    LOG_DEBUG("[ChronoKVS] Retrieved {} events for key='{}' in range=[{}, {})",
              events.size(), key, start_timestamp, end_timestamp);
    return events;
}

std::optional<EventData> ChronoKVSMapper::retrieveEarliestByKey(const std::string& key)
{
    LOG_DEBUG("[ChronoKVS] Retrieving earliest event for key='{}'", key);

    // Retrieve all events for the given key and return the one with the earliest timestamp.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        LOG_DEBUG("[ChronoKVS] No events found for key='{}'", key);
        return std::nullopt;
    }

    // Find the event with the minimum timestamp.
    // Note: We use min_element because the underlying storage does not guarantee
    // events are returned in timestamp order.
    auto earliest = std::min_element(events.begin(),
                                     events.end(),
                                     [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    LOG_DEBUG("[ChronoKVS] Found earliest event for key='{}' with timestamp={}", key, earliest->timestamp);
    return *earliest;
}

std::optional<EventData> ChronoKVSMapper::retrieveLatestByKey(const std::string& key)
{
    LOG_DEBUG("[ChronoKVS] Retrieving latest event for key='{}'", key);

    // Retrieve all events for the given key and return the one with the latest timestamp.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        LOG_DEBUG("[ChronoKVS] No events found for key='{}'", key);
        return std::nullopt;
    }

    // Find the event with the maximum timestamp.
    // Note: We use max_element because the underlying storage does not guarantee
    // events are returned in timestamp order.
    auto latest = std::max_element(events.begin(),
                                   events.end(),
                                   [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    LOG_DEBUG("[ChronoKVS] Found latest event for key='{}' with timestamp={}", key, latest->timestamp);
    return *latest;
}

} // namespace chronokvs
