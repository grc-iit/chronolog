#include "chronokvs_mapper.h"
#include "chronokvs_utils.h"

namespace chronokvs
{
chronokvs_mapper::chronokvs_mapper()
{
    memoryManager = std::make_unique <MemoryManager>();
    chronoClient = std::make_unique <ChronologClient>();
}

std::uint64_t chronokvs_mapper::storeKeyValue(const std::string &key, const std::string &value)
{
    std::string serialized = serialize(key, value);
    std::uint64_t timestamp = chronoClient->storeEvent(serialized);
    memoryManager->store(key, timestamp);
    return timestamp;
}

std::vector <std::pair <std::string, std::string>> chronokvs_mapper::retrieveByTimestamp(std::uint64_t timestamp)
{
    std::vector <std::string> serializedEvents = chronoClient->retrieveEvents(timestamp);
    std::vector <std::pair <std::string, std::string>> keyValues;

    keyValues.reserve(serializedEvents.size());
    for(const std::string &serializedEvent: serializedEvents)
    {
        keyValues.push_back(deserialize(serializedEvent));
    }
    return keyValues;
}

std::vector <std::pair <std::uint64_t, std::string>> chronokvs_mapper::retrieveByKey(const std::string &key)
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

std::vector <std::string> chronokvs_mapper::retrieveByKeyAndTimestamp(const std::string &key, std::uint64_t timestamp)
{
    std::vector <std::string> serializedEvents = chronoClient->retrieveEvents(timestamp);
    std::vector <std::string> values;

    for(const std::string &serializedEvent: serializedEvents)
    {
        auto keyValue = deserialize(serializedEvent);
        if(keyValue.first == key)
        {
            // Add the value to values vector
            values.push_back(keyValue.second);
        }
    }
    return values;
}
}