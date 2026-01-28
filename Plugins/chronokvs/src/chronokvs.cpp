#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "chronokvs.h"
#include "chronokvs_mapper.h"

namespace chronokvs
{

ChronoKVS::ChronoKVS()
    : mapper(std::make_unique<ChronoKVSMapper>()) // Use member initializer list
{}

ChronoKVS::~ChronoKVS() = default;

std::uint64_t ChronoKVS::put(const std::string& key, const std::string& value)
{
    std::cerr << "[ChronoKVS] put() called for key='" << key << "' (value_size=" << value.size() << ")" << std::endl;
    return mapper->storeKeyValue(key, value);
}

std::string ChronoKVS::get(const std::string& key, std::uint64_t timestamp)
{
    std::cerr << "[ChronoKVS] get() called for key='" << key << "' at timestamp=" << timestamp << std::endl;
    return mapper->retrieveByKeyAndTs(key, timestamp);
}

std::vector<EventData> ChronoKVS::get_history(const std::string& key)
{
    std::cerr << "[ChronoKVS] get_history() called for key='" << key << "'" << std::endl;
    return mapper->retrieveByKey(key);
}

std::vector<EventData>
ChronoKVS::get_range(const std::string& key, std::uint64_t start_timestamp, std::uint64_t end_timestamp)
{
    std::cerr << "[ChronoKVS] get_range() called for key='" << key << "' range=[" << start_timestamp << ", "
              << end_timestamp << ")" << std::endl;
    return mapper->retrieveByKeyAndRange(key, start_timestamp, end_timestamp);
}

std::optional<EventData> ChronoKVS::get_earliest(const std::string& key) { return mapper->retrieveEarliestByKey(key); }

std::optional<EventData> ChronoKVS::get_latest(const std::string& key) { return mapper->retrieveLatestByKey(key); }

} // namespace chronokvs
