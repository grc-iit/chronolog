#include "chronolog_client.h"

namespace chronolog
{

// Store an event in ChronoLog and return a timestamp
std::uint64_t ChronoLogClient::storeEvent(const std::string &serializedEvent)
{
    std::uint64_t timestamp = ++currentTimestamp; // Increment to simulate timestamp generation
    events[timestamp] = serializedEvent;
    return timestamp;
}

// Retrieve an event from ChronoLog using a timestamp
std::vector <std::string> ChronoLogClient::retrieveEvents(std::uint64_t timestamp)
{
    std::vector <std::string> matchingEvents;

    // Iterate through the events container (assuming it's a container supporting iteration)
    for(const auto &eventPair: events)
    {
        if(eventPair.first == timestamp)
        {
            matchingEvents.push_back(eventPair.second);
        }
    }
    return matchingEvents;
}

} // namespace chronolog
