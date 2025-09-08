#include "chronokvs_mapper.h"

namespace chronokvs
{

ChronoKVSMapper::ChronoKVSMapper()
{
    chronoClientAdapter = std::make_unique<ChronoKVSClientAdapter>();
}

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string &key, const std::string &value)
{
    // TODO: Implement key-value storage
    return chronoClientAdapter->storeEvent(value);
}

std::vector<std::pair<std::uint64_t, std::string>> ChronoKVSMapper::retrieveByKey(const std::string &key)
{
    std::vector<std::pair<std::uint64_t, std::string>> results;
    // TODO: Implement key-based retrieval
    return results;
}

std::string ChronoKVSMapper::retrieveByKeyAndTs(const std::string &key, std::uint64_t timestamp)
{
    //TODO: Implement key-value retrieval
    std::vector<std::string> events = chronoClientAdapter->retrieveEvents(timestamp);
    if (!events.empty()) {
        return events[0];
    }
    return "";
}

}