#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "chronokvs_mapper.h"

namespace chronokvs
{

// Constants for time range boundaries (used across multiple retrieval methods)
constexpr uint64_t MIN_TIMESTAMP = 1;          // Earliest possible timestamp
constexpr uint64_t MAX_TIMESTAMP = UINT64_MAX; // Maximum possible uint64_t value

ChronoKVSMapper::ChronoKVSMapper() { chronoClientAdapter = std::make_unique<ChronoKVSClientAdapter>(); }

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string& key, const std::string& value)
{
    // Input validation
    if(key.empty())
    {
        std::cerr << "[ChronoKVS] Invalid input: key cannot be empty" << std::endl;
        throw std::invalid_argument("Key cannot be empty");
    }
    if(value.empty())
    {
        std::cerr << "[ChronoKVS] Invalid input: value cannot be empty for key='" << key << "'" << std::endl;
        throw std::invalid_argument("Value cannot be empty");
    }

    std::cerr << "[ChronoKVS] Storing key-value pair: key='" << key << "', value_size=" << value.size() << std::endl;
    return chronoClientAdapter->storeEvent(key, value);
}

std::string ChronoKVSMapper::retrieveByKeyAndTs(const std::string& key, std::uint64_t timestamp)
{
    std::cerr << "[ChronoKVS] Retrieving value for key='" << key << "' at timestamp=" << timestamp << std::endl;

    // Retrieve events in the narrow time range [timestamp, timestamp + 1)
    auto events = chronoClientAdapter->retrieveEvents(key, timestamp, timestamp + 1);

    if(events.empty())
    {
        std::cerr << "[ChronoKVS] No events found for key='" << key << "' at timestamp=" << timestamp << std::endl;
        return "";
    }

    // Return the first event's value only if its timestamp matches the requested timestamp
    if(events[0].timestamp == timestamp)
    {
        std::cerr << "[ChronoKVS] Found event for key='" << key << "' at timestamp=" << timestamp << std::endl;
        return events[0].value;
    }
    std::cerr << "[ChronoKVS] No event found for key='" << key << "' at exact timestamp=" << timestamp << std::endl;
    return "";
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string& key)
{
    // Retrieve all events for the given key using the full time range.
    // This ensures we capture all events regardless of their timestamp,
    // avoiding any potential data loss from timing uncertainties.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
    std::cerr << "[ChronoKVS] Retrieved " << events.size() << " events for key='" << key << "'" << std::endl;
    return events;
}

std::vector<EventData> ChronoKVSMapper::retrieveByKeyAndRange(const std::string& key,
                                                              std::uint64_t start_timestamp,
                                                              std::uint64_t end_timestamp)
{
    // Validate timestamp range
    if(start_timestamp >= end_timestamp)
    {
        std::cerr << "[ChronoKVS] Invalid timestamp range for key='" << key << "': start_timestamp=" << start_timestamp
                  << " >= end_timestamp=" << end_timestamp << std::endl;
        throw std::invalid_argument("Invalid timestamp range: start_timestamp must be less than end_timestamp");
    }

    std::cerr << "[ChronoKVS] Retrieving events for key='" << key << "' range=[" << start_timestamp << ", "
              << end_timestamp << ")" << std::endl;

    // Retrieve events for the given key within the specified time range [start_timestamp, end_timestamp).
    auto events = chronoClientAdapter->retrieveEvents(key, start_timestamp, end_timestamp);
    std::cerr << "[ChronoKVS] Retrieved " << events.size() << " events for key='" << key << "' in range=["
              << start_timestamp << ", " << end_timestamp << ")" << std::endl;
    return events;
}

std::optional<EventData> ChronoKVSMapper::retrieveEarliestByKey(const std::string& key)
{
    // Retrieve all events for the given key and return the one with the earliest timestamp.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        return std::nullopt;
    }

    // Find the event with the minimum timestamp.
    // Note: We use min_element because the underlying storage does not guarantee
    // events are returned in timestamp order.
    auto earliest = std::min_element(events.begin(),
                                     events.end(),
                                     [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    return *earliest;
}

std::optional<EventData> ChronoKVSMapper::retrieveLatestByKey(const std::string& key)
{
    // Retrieve all events for the given key and return the one with the latest timestamp.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        return std::nullopt;
    }

    // Find the event with the maximum timestamp.
    // Note: We use max_element because the underlying storage does not guarantee
    // events are returned in timestamp order.
    auto latest = std::max_element(events.begin(),
                                   events.end(),
                                   [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    return *latest;
}

} // namespace chronokvs
