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
    // Query with a half-open range [timestamp, timestamp + 1)
    // This ensures we capture exactly the timestamp we want
    uint64_t start_time = 1;
    uint64_t end_time = 2000000000000000000; // May 18, 2033, 03:33:20 UTC
    auto events = chronoClientAdapter->retrieveEvents(key, start_time, end_time);
    
    if (events.empty()) {
        return ""; // No value exists for this timestamp
    }
    
    // Find the exact timestamp match
    for (const auto& event : events) {
        if (event.timestamp == timestamp) {
            return event.value;
        }
    }
    
    return ""; // No exact match found
}

std::vector<EventData> ChronoKVSMapper::retrieveByKey(const std::string &key)
{
    // Use an intentionally wide time range to ensure all events are captured.
    // 'start_time' is set to 1 to include any valid timestamp from the beginning.
    // 'end_time' is set to a very large value to ensure we read up to the latest
    // events. This approach avoids missing any data due to timing uncertainties.
    uint64_t start_time = 1;
    uint64_t end_time = 2000000000000000000; // May 18, 2033, 03:33:20 UTC
    return chronoClientAdapter->retrieveEvents(key, start_time, end_time);
}

}