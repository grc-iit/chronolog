#include <iostream>
#include "chronokvs.h"
#include "chronokvs_mapper.h"

namespace chronokvs
{

ChronoKVS::ChronoKVS()
{
    mapper = std::make_unique<ChronoKVSMapper>();
}

ChronoKVS::~ChronoKVS() = default;

std::uint64_t ChronoKVS::put(const std::string &key, const std::string &value)
{
    return mapper->storeKeyValue(key, value);
}

std::string ChronoKVS::get(const std::string &key, std::uint64_t timestamp)
{
    return mapper->retrieveByKeyAndTs(key, timestamp);
}

std::vector<EventData> ChronoKVS::get_history(const std::string &key)
{
    return mapper->retrieveByKey(key);
}
}
