#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chronokvs.h"
#include "chronokvs_mapper.h"

namespace chronokvs
{

ChronoKVS::ChronoKVS(LogLevel level)
    : mapper(std::make_unique<ChronoKVSMapper>(level))
    , logLevel_(level)
{}

ChronoKVS::~ChronoKVS() = default;

std::uint64_t ChronoKVS::put(const std::string& key, const std::string& value)
{
    CHRONOKVS_DEBUG(logLevel_, "put() called for key='", key, "' (value_size=", value.size(), ")");
    return mapper->storeKeyValue(key, value);
}

std::string ChronoKVS::get(const std::string& key, std::uint64_t timestamp)
{
    CHRONOKVS_DEBUG(logLevel_, "get() called for key='", key, "' at timestamp=", timestamp);
    return mapper->retrieveByKeyAndTs(key, timestamp);
}

std::vector<EventData> ChronoKVS::get_history(const std::string& key)
{
    CHRONOKVS_DEBUG(logLevel_, "get_history() called for key='", key, "'");
    return mapper->retrieveByKey(key);
}

std::vector<EventData>
ChronoKVS::get_range(const std::string& key, std::uint64_t start_timestamp, std::uint64_t end_timestamp)
{
    CHRONOKVS_DEBUG(logLevel_, "get_range() called for key='", key, "' range=[", start_timestamp, ", ", end_timestamp, ")");
    return mapper->retrieveByKeyAndRange(key, start_timestamp, end_timestamp);
}

std::optional<EventData> ChronoKVS::get_earliest(const std::string& key) { return mapper->retrieveEarliestByKey(key); }

std::optional<EventData> ChronoKVS::get_latest(const std::string& key) { return mapper->retrieveLatestByKey(key); }

} // namespace chronokvs
