#include "chronokvs_mapper.h"

namespace chronokvs
{

// Constants for time range boundaries
constexpr uint64_t MIN_TIMESTAMP = 1;  // Earliest possible timestamp
constexpr uint64_t MAX_TIMESTAMP = 2000000000000000000;  // ~May 18, 2033 03:33:20 UTC

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
    // TODO(Performance): Current implementation fetches all events due to issues with narrow time ranges
    // causing timeouts. This should be optimized to use [timestamp, timestamp + 1) once the underlying
    // timing issues are resolved.
    auto events = chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
    
    if (events.empty()) {
        return "";
    }
    
    // Search for exact timestamp match in the retrieved events
    for (const auto& event : events) {
        if (event.timestamp == timestamp) {
            return event.value;
        }
    }
    
    return "";
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string &key)
{
    // Retrieve all events for the given key using the full time range.
    // This ensures we capture all events regardless of their timestamp,
    // avoiding any potential data loss from timing uncertainties.
    return chronoClientAdapter->retrieveEvents(key, MIN_TIMESTAMP, MAX_TIMESTAMP);
}

}