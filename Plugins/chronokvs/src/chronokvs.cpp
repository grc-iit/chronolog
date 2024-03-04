#include <iostream>
#include "chronokvs.h"
#include "chronokvs_mapper.h"

namespace chronolog
{
chronokvs_mapper mapper;

ChronoKVS::ChronoKVS()
{
    std::cout << "Entering ChronoLogKVS Constructor" << std::endl;
    // Initialization code here
    std::cout << "Exiting ChronoLogKVS Constructor" << std::endl;
}

ChronoKVS::~ChronoKVS()
{
    std::cout << "Entering ChronoLogKVS Destructor" << std::endl;
    // Cleanup code here
    std::cout << "Exiting ChronoLogKVS Destructor" << std::endl;
}

std::uint64_t ChronoKVS::put(const std::string &key, const std::string &value)
{
    return mapper.storeKeyValue(key, value);
}

std::vector <std::pair <std::string, std::string>> ChronoKVS::get(std::uint64_t timestamp)
{
    return mapper.retrieveByTimestamp(timestamp);
}

std::vector <std::pair <std::uint64_t, std::string>> ChronoKVS::get(const std::string &key)
{
    return mapper.retrieveByKey(key);
}

std::vector <std::string> ChronoKVS::get(const std::string &key, std::uint64_t timestamp)
{
    return mapper.retrieveByKeyAndTimestamp(key, timestamp);
}

} // namespace chronolog
