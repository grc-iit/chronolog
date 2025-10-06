#include "chronokvs_mapper.h"

namespace chronokvs
{


ChronoKVSMapper::ChronoKVSMapper()
{
    chronoClientAdapter = std::make_unique<ChronoKVSClientAdapter>();
}

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string &key, const std::string &value)
{
    return chronoClientAdapter->storeEvent(key, value);
}

std::string ChronoKVSMapper::retrieveByKeyAndTs(const std::string &key, std::uint64_t timestamp)
{
    // Retrieve events in the narrow time range [timestamp, timestamp + 1)
    auto events = chronoClientAdapter->retrieveEvents(key, timestamp, timestamp + 1);
    
    if (events.empty()) {
        return "";
    }
    
    // Return the first event's value only if its timestamp matches the requested timestamp
    if (events[0].timestamp == timestamp) {
        return events[0].value;
    }
    return "";
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string &key)
{
    // Constants for time range boundaries
    constexpr uint64_t MIN_TIMESTAMP = 1;  // Earliest possible timestamp
    constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000;  // ~May 18, 2033 03:33:20 UTC
    
    // Retrieve all events for the given key using the full time range.
    // This ensures we capture all events regardless of their timestamp,
    // avoiding any potential data loss from timing uncertainties.
    return chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
}

}