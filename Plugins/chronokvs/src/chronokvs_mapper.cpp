#include <cstdint>
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

} // namespace chronokvs
