//
// Created by eneko on 2/20/24.
//

#ifndef CHRONOLOG_CLIENT_H_
#define CHRONOLOG_CLIENT_H_

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

namespace chronolog
{

/**
 * Client for interacting with the ChronoLog system.
 */
class ChronoLogClient
{
private:
    std::unordered_map <std::uint64_t, std::string> events; // Simulates the ChronoLog storage
    std::uint64_t currentTimestamp = 0; // Simplified timestamp generation
public:
    ChronoLogClient() = default;
    ~ChronoLogClient() = default;

    // Store an event in ChronoLog and return a timestamp
    std::uint64_t storeEvent(const std::string &serializedEvent);

    // Retrieve an event from ChronoLog using a timestamp
    std::vector <std::string> retrieveEvents(std::uint64_t timestamp);

};
}

#endif // CHRONOLOG_CLIENT_H_
