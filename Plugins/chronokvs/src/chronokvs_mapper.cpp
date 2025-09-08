#include "chronokvs_mapper.h"

namespace chronokvs
{
ChronoKVSMapper::ChronoKVSMapper()
{
    chronoClientAdapter = std::make_unique <ChronoKVSClientAdapter>();
}

std::uint64_t ChronoKVSMapper::storeKeyValue(const std::string &key, const std::string &value)
{
    std::string serialized = serialize(key, value);
    std::uint64_t timestamp = chronoClient->storeEvent(serialized);
    return timestamp;
}

std::vector <std::pair <std::uint64_t, std::string>> ChronoKVSMapper::retrieveByKey(const std::string &key)
{
    std::vector <std::uint64_t> timestamps = memoryManager->retrieveByKey(key);
    std::vector <std::pair <std::uint64_t, std::string>> results;

    for(const auto &timestamp: timestamps)
    {
        std::vector <std::string> serializedEvents = chronoClient->retrieveEvents(timestamp);

        for(const std::string &serializedEvent: serializedEvents)
        {
            auto keyValue = deserialize(serializedEvent);
            if(keyValue.first == key)
            {
                // Add the timestamp-value pair to the results
                results.emplace_back(timestamp, keyValue.second);
            }
        }
    }
    return results;
}

std::string ChronoKVSMapper::retrieveByKeyAndTimestamp(const std::string &key, std::uint64_t timestamp)
{
    std::vector <std::string> serializedEvents = chronoClient->retrieveEvents(timestamp);
    std::string value;

    for(const std::string &serializedEvent: serializedEvents)
    {
        auto keyValue = deserialize(serializedEvent);
        if(keyValue.first == key)
        {
            // Add the value to values vector
            value = keyValue.second;
            break;
        }
    }
    return value;
}

}