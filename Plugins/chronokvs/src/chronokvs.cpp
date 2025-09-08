#include <iostream>
#include "chronokvs.h"
#include "chronokvs_mapper.h"

namespace chronokvs
{

ChronoKVS::ChronoKVS()
{
    mapper = std::make_unique <chronokvs_mapper>();
}

std::uint64_t ChronoKVS::put(const std::string &key, const std::string &value)
{
    return mapper->storeKeyValue(key, value);
}

std::vector <std::pair <std::uint64_t, std::string>> ChronoKVS::get_history(const std::string &key)
{
    return mapper->retrieveByKey(key);
}

std::string ChronoKVS::get(const std::string &key, std::uint64_t timestamp)
{
    return mapper->retrieveByKeyAndTs(key, timestamp);
}
}
