//
// Created by eneko on 2/19/24.
//
#include "chronokvs.h"
#include "chronokvs_mapper.h"
#include <iostream>
// Include other necessary headers

namespace chronolog
{

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
    std::cout << "Entering put() method with key: " << key << " and value: " << value << std::endl;

    // Assume a timestamp is generated for each put operation.
    std::uint64_t timestamp = 0; // Placeholder for the actual timestamp generation logic.
    // Implementation for storing the key-value pair along with the timestamp.

    std::cout << "Exiting put() method" << std::endl;
    return timestamp;
}

std::vector <std::pair <std::string, std::string>> ChronoKVS::get(uint64_t timestamp)
{


    // Placeholder for the return value.
    std::vector <std::pair <std::string, std::string>> keyValuePairs;
    // Implementation for retrieving key-value pairs at the given timestamp.

    return keyValuePairs;
}

std::vector <std::pair <uint64_t, std::string>> ChronoKVS::get(const std::string &key)
{


    // Placeholder for the return value.
    std::vector <std::pair <uint64_t, std::string>> timestampsAndValues;
    // Implementation for retrieving all timestamps and values for a given key.

    return timestampsAndValues;
}

std::string ChronoKVS::get(const std::string &key, uint64_t timestamp)
{

    // Placeholder for the return value.
    std::string value = ""; // Assume an empty string if no value is found.
    // Implementation for retrieving a value for a given key at a specific timestamp.

    return value;
}

} // namespace chronolog
