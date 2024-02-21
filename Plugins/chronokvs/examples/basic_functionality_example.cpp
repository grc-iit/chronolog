//
// Created by eneko on 2/21/24.
//

#include "chronokvs.h"
#include <iostream>
#include <string>

int main()
{
    // Note: ChronoKVS::put is now a static method that returns a timestamp.
    std::string key1 = "key1";
    std::string value1 = "value1";

    std::string key2 = "key2";
    std::string value2 = "value2";

    // Put operation with current timestamp
    std::cout << "Putting key-value pairs into ChronoKVS...\n";
    std::uint64_t timestamp1 = chronolog::ChronoKVS::put(key1, value1);
    std::uint64_t timestamp2 = chronolog::ChronoKVS::put(key2, value2);

    // Retrieve operation by timestamp
    std::cout << "Retrieving values by timestamp from ChronoKVS...\n";
    auto valuesAtTimestamp1 = chronolog::ChronoKVS::get(timestamp1);
    for(const auto &pair: valuesAtTimestamp1)
    {
        std::cout << "At timestamp " << timestamp1 << ", key: " << pair.first << " value: " << pair.second << "\n";
    }

    // Retrieve all timestamps and values for a key
    std::cout << "Retrieving all timestamps and values for a key...\n";
    auto historyForKey2 = chronolog::ChronoKVS::get(key2);
    for(const auto &pair: historyForKey2)
    {
        std::cout << "Key " << key2 << " at timestamp " << pair.first << " had value: " << pair.second << "\n";
    }

    // Retrieve value by key and specific timestamp
    std::cout << "Retrieving value by key and specific timestamp...\n";
    auto valueForKey1AtTimestamp = chronolog::ChronoKVS::get(key1, timestamp1);
    if(!valueForKey1AtTimestamp.empty())
    {
        std::cout << "Key " << key1 << " at timestamp " << timestamp1 << " had value: " << valueForKey1AtTimestamp[0]
                  << "\n";
    }
    else
    {
        std::cout << "No value found for " << key1 << " at timestamp " << timestamp1 << "\n";
    }

    return 0;
}
