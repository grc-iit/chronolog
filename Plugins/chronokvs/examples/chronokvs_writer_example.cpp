#include <string>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include "chronokvs.h"

int main()
{
    try {
        // Create ChronoKVS instance
        chronokvs::ChronoKVS chronoKVS;

        // Define key-value pairs
        std::string key1 = "key1";
        std::string value1 = "value1";
        std::string key2 = "key2";
        std::string value2 = "value2";
        std::string value3 = "value3";

        // Test error handling with invalid input
        try {
            std::cout << "Testing error handling with empty key...\n";
            chronoKVS.put("", "test");
        } catch (const std::invalid_argument& e) {
            std::cout << "Successfully caught invalid argument: " << e.what() << "\n\n";
        }

        // Insert key-value pairs and store timestamps
        std::cout << "Putting key-value pairs into ChronoKVS...\n";
        
        std::uint64_t timestamp1 = chronoKVS.put(key1, value1);
        std::cout << "1. Successfully stored key1=" << value1 << " timestamp=" << timestamp1 << "\n";
        
        std::uint64_t timestamp2 = chronoKVS.put(key2, value2);
        std::cout << "2. Successfully stored key2=" << value2 << " timestamp=" << timestamp2 << "\n";
        
        std::uint64_t timestamp3 = chronoKVS.put(key2, value3);
        std::cout << "3. Successfully stored key2=" << value3 << " timestamp=" << timestamp3 << "\n";

        // Save timestamps to a file for the reader
        std::ofstream timestampFile("chronokvs_timestamps.txt");
        if (!timestampFile.is_open()) {
            throw std::runtime_error("Could not open chronokvs_timestamps.txt for writing");
        }

        timestampFile << timestamp1 << "\n"
                     << timestamp2 << "\n"
                     << timestamp3 << "\n";
        timestampFile.close();
        std::cout << "\nTimestamps have been saved to 'chronokvs_timestamps.txt'\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
