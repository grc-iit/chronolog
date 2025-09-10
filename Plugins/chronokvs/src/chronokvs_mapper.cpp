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
    // Query for exact timestamp match
    auto events = chronoClientAdapter->retrieveEvents(key, timestamp, timestamp);
    
    if (events.empty()) {
        return ""; // No value exists for this timestamp
    }
    
    if (events.size() > 1) {
        throw std::runtime_error("Unexpected multiple events found for single timestamp");
    }
    
    // At this point we have exactly one event
    return events[0].value;
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