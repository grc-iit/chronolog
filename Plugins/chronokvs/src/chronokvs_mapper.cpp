#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "chronokvs_mapper.h"

namespace chronokvs
{


ChronoKVSMapper::ChronoKVSMapper() { chronoClientAdapter = std::make_unique<ChronoKVSClientAdapter>(); }

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string& key, const std::string& value)
{
    return chronoClientAdapter->storeEvent(key, value);
}

std::string ChronoKVSMapper::retrieveByKeyAndTs(const std::string& key, std::uint64_t timestamp)
{
    // Retrieve events in the narrow time range [timestamp, timestamp + 1)
    auto events = chronoClientAdapter->retrieveEvents(key, timestamp, timestamp + 1);

    if(events.empty())
    {
        return "";
    }

    // Return the first event's value only if its timestamp matches the requested timestamp
    if(events[0].timestamp == timestamp)
    {
        return events[0].value;
    }
    return "";
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string& key)
{
    // Constants for time range boundaries
    constexpr uint64_t MIN_TIMESTAMP = 1;                   // Earliest possible timestamp
    constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000; // ~May 18, 2033 03:33:20 UTC

    // Retrieve all events for the given key using the full time range.
    // This ensures we capture all events regardless of their timestamp,
    // avoiding any potential data loss from timing uncertainties.
    return chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
}

std::vector<EventData> ChronoKVSMapper::retrieveByKeyAndRange(const std::string& key,
                                                              std::uint64_t start_timestamp,
                                                              std::uint64_t end_timestamp)
{
    // Retrieve events for the given key within the specified time range [start_timestamp, end_timestamp).
    // If the range is empty or invalid (start_timestamp >= end_timestamp), return no events.
    if(start_timestamp >= end_timestamp)
    {
        return std::vector<EventData>{};
    }
    return chronoClientAdapter->retrieveEvents(key, start_timestamp, end_timestamp);
}

std::optional<EventData> ChronoKVSMapper::retrieveEarliestByKey(const std::string& key)
{
    // Retrieve all events for the given key and return the one with the earliest timestamp.
    // Constants for time range boundaries
    constexpr uint64_t MIN_TIMESTAMP = 1;                   // Earliest possible timestamp
    constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000; // ~May 18, 2033 03:33:20 UTC

    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        return std::nullopt;
    }

    // Find the event with the minimum timestamp
    auto earliest = std::min_element(events.begin(),
                                     events.end(),
                                     [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    return *earliest;
}

std::optional<EventData> ChronoKVSMapper::retrieveLatestByKey(const std::string& key)
{
    // Retrieve all events for the given key and return the one with the latest timestamp.
    // Constants for time range boundaries
    constexpr uint64_t MIN_TIMESTAMP = 1;                   // Earliest possible timestamp
    constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000; // ~May 18, 2033 03:33:20 UTC

    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);

    if(events.empty())
    {
        return std::nullopt;
    }

    // Find the event with the maximum timestamp
    auto latest = std::max_element(events.begin(),
                                  events.end(),
                                  [](const EventData& a, const EventData& b) { return a.timestamp < b.timestamp; });

    return *latest;
}

} // namespace chronokvs