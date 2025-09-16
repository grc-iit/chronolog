#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include "chronokvs.h"

std::vector<std::uint64_t> readTimestampsFromFile(const std::string& filename) {
    std::vector<std::uint64_t> timestamps;
    std::ifstream file(filename);
    std::uint64_t timestamp;
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open " << filename << "\n";
        return timestamps;
    }

    while (file >> timestamp) {
        timestamps.push_back(timestamp);
    }
    
    file.close();
    return timestamps;
}

int main()
{
    // Create ChronoKVS instance
    chronokvs::ChronoKVS chronoKVS;

    // Read timestamps from file
    std::vector<std::uint64_t> timestamps = readTimestampsFromFile("chronokvs_timestamps.txt");
    if (timestamps.empty()) {
        std::cerr << "No timestamps found. Please run the writer example first.\n";
        return 1;
    }

    if (timestamps.size() != 3) {
        std::cerr << "Expected 3 timestamps, but found " << timestamps.size() << "\n";
        return 1;
    }

    std::string key1 = "key1";
    std::string key2 = "key2";

    // Read and display history for key2
    std::cout << "\nReading history for key2:\n";
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
    std::string value2_t1 = chronoKVS.get(key2, timestamps[1]);
    std::string value2_t2 = chronoKVS.get(key2, timestamps[2]);

    std::cout << "Value at timestamp " << timestamps[1] << ": " 
              << (value2_t1.empty() ? "Not found" : value2_t1) << "\n";
    std::cout << "Value at timestamp " << timestamps[2] << ": " 
              << (value2_t2.empty() ? "Not found" : value2_t2) << "\n";    

    // Read and display value for key1
    std::cout << "\nReading value for key1 at timestamp " << timestamps[0] << ":\n";
    std::string value1 = chronoKVS.get(key1, timestamps[0]);
    if (!value1.empty()) {
        std::cout << "Value: " << value1 << "\n";
    } else {
        std::cout << "No value found\n";
    }

    return 0;
   
}