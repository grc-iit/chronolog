#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include "chronokvs.h"

std::vector<std::uint64_t> readTimestampsFromFile(const std::string& filename) {
    std::vector<std::uint64_t> timestamps;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        throw std::runtime_error("Could not open " + filename);
    }

    std::uint64_t timestamp;
    while (file >> timestamp) {
        timestamps.push_back(timestamp);
    }
    
    file.close();

    if (timestamps.empty()) {
        throw std::runtime_error("No timestamps found in " + filename);
    }

    if (timestamps.size() != 3) {
        throw std::runtime_error("Expected 3 timestamps, but found " + std::to_string(timestamps.size()));
    }

    return timestamps;
}

int main()
{
    try {
        // Create ChronoKVS instance
        chronokvs::ChronoKVS chronoKVS;

        // Read timestamps from file
        std::cout << "Reading timestamps from file...\n";
        std::vector<std::uint64_t> timestamps = readTimestampsFromFile("chronokvs_timestamps.txt");
        std::cout << "Successfully read " << timestamps.size() << " timestamps\n\n";

        std::string key1 = "key1";
        std::string key2 = "key2";

        // Test error handling with invalid input
        try {
            std::cout << "Testing error handling with empty key...\n";
            chronoKVS.get_history("");
        } catch (const std::invalid_argument& e) {
            std::cout << "Successfully caught invalid argument: " << e.what() << "\n\n";
        }

        // Read and display history for key2
        std::cout << "Reading history for key2:\n";
        auto historyForKey2 = chronoKVS.get_history(key2);
        
        if (historyForKey2.empty()) {
            std::cout << "No history found for key2\n";
        } else {
            std::cout << "Found " << historyForKey2.size() << " values in history:\n";
            for (const auto& event : historyForKey2) {
                std::cout << "Timestamp: " << event.timestamp 
                         << "\nValue    : " << event.value << "\n\n";
            }
        }

        // Read specific values for key2 at both timestamps
        std::cout << "Reading specific values for key2:\n";
        try {
            std::string value2_t1 = chronoKVS.get(key2, timestamps[1]);
            std::cout << "Value at timestamp " << timestamps[1] << ": " << value2_t1 << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Failed to read value at timestamp " << timestamps[1] << ": " << e.what() << "\n";
        }

        try {
            std::string value2_t2 = chronoKVS.get(key2, timestamps[2]);
            std::cout << "Value at timestamp " << timestamps[2] << ": " << value2_t2 << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Failed to read value at timestamp " << timestamps[2] << ": " << e.what() << "\n";
        }

        // Read and display value for key1
        std::cout << "\nReading value for key1 at timestamp " << timestamps[0] << ":\n";
        try {
            std::string value1 = chronoKVS.get(key1, timestamps[0]);
            std::cout << "Value: " << value1 << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Failed to read value: " << e.what() << "\n";
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
