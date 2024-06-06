#include <string>
#include <iostream>
#include "chronokvs.h"

int main()
{
    // Phase 0: ChronoKVS Instance Creation
    chronokvs::ChronoKVS ChronoKVS; // Assuming ChronoKVS is in the namespace chronokvs and has a default constructor=

    // Phase 1: Variable Definition
    std::string key1 = "key1";
    std::string value1 = "value1";
    std::string key2 = "key2";
    std::string value2 = "value2";

    // Phase 2: Key-Value Insertion
    std::cout << "Putting key-value pairs into ChronoKVS...\n";
    std::uint64_t timestamp1 = ChronoKVS.put(key1, value1);
    std::uint64_t timestamp2 = ChronoKVS.put(key2, value2);

    // Phase 3: Retrieval By Timestamp
    std::cout << "Retrieving values by timestamp from ChronoKVS...\n";
    auto valuesAtTimestamp1 = ChronoKVS.get(timestamp1);
    for(const auto &pair: valuesAtTimestamp1)
    {
        std::cout << "At timestamp " << timestamp1 << ", key: " << pair.first << " value: " << pair.second << "\n";
    }

    // Phase 4: Retrieving History For A Key
    std::cout << "Retrieving all timestamps and values for a key...\n";
    auto historyForKey2 = ChronoKVS.get(key2);
    for(const auto &pair: historyForKey2)
    {
        std::cout << "Key " << key2 << " at timestamp " << pair.first << " had value: " << pair.second << "\n";
    }

    // Phase 5: Retrieving Value By Key And Specific Timestamp
    std::cout << "Retrieving value by key and specific timestamp...\n";
    auto valueForKey1AtTimestamp = ChronoKVS.get(key1, timestamp1);
    if(!valueForKey1AtTimestamp.empty())
    {
        std::cout << "Key " << key1 << " at timestamp " << timestamp1 << " had value: " << valueForKey1AtTimestamp
                  << "\n";
    }
    else
    {
        std::cout << "No value found for " << key1 << " at timestamp " << timestamp1 << "\n";
    }
    return 0;
}
