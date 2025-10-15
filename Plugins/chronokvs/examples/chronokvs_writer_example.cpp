#include <cstdint>
#include <string>
#include <iostream>
#include <fstream>
#include "chronokvs.h"

int main()
{
    // Create ChronoKVS instance
    chronokvs::ChronoKVS chronoKVS;

    // Define key-value pairs
    std::string key1 = "key1";
    std::string value1 = "value1";
    std::string key2 = "key2";
    std::string value2 = "value2";
    std::string value3 = "value3";

    // Insert key-value pairs and store timestamps
    std::cout << "Putting key-value pairs into ChronoKVS...\n";
    std::uint64_t timestamp1 = chronoKVS.put(key1, value1);
    std::uint64_t timestamp2 = chronoKVS.put(key2, value2);
    std::uint64_t timestamp3 = chronoKVS.put(key2, value3);
    
    std::cout << "Inserted values with timestamps:\n";
    std::cout << "1. key1=" << value1 << " timestamp=" << timestamp1 << "\n";
    std::cout << "2. key2=" << value2 << " timestamp=" << timestamp2 << "\n";
    std::cout << "3. key2=" << value3 << " timestamp=" << timestamp3 << "\n";

    // Save timestamps to a file for the reader
    std::ofstream timestampFile("chronokvs_timestamps.txt");
    if (timestampFile.is_open()) {
        timestampFile << timestamp1 << "\n";
        timestampFile << timestamp2 << "\n";
        timestampFile << timestamp3 << "\n";
        timestampFile.close();
        std::cout << "\nTimestamps have been saved to 'chronokvs_timestamps.txt'\n";
    } else {
        std::cerr << "Error: Could not save timestamps to file\n";
    }

    return 0;
}