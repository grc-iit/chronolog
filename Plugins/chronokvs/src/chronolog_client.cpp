#include "chronolog_client.h"

namespace chronolog
{
ChronoLogClient::ChronoLogClient()
{
    int flags = 0;
    chronolog = std::make_shared <MockChronolog>();
    chronolog->Connect();
    chronolog->CreateChronicle("defaultChronicle", {}, flags);
    auto [status, handle] = chronolog->AcquireStory("defaultChronicle", "defaultStory", {}, flags);
    storyHandle = handle;
}

ChronoLogClient::~ChronoLogClient()
{
    if(storyHandle != nullptr)
    {
        chronolog->ReleaseStory("defaultChronicle", "defaultStory");
        chronolog->DestroyStory("defaultChronicle", "defaultStory");
    }
    chronolog->DestroyChronicle("defaultChronicle");
    chronolog->Disconnect();
}

// Store an event in ChronoLog and return a timestamp
std::uint64_t ChronoLogClient::storeEvent(const std::string &serializedEvent)
{
    if(storyHandle == nullptr)
    {
        return 0; // Error handling or re-acquire the story handle if necessary
    }

    // Record the event using the persistent story handle
    std::uint64_t eventTimestamp = storyHandle->record(serializedEvent);

    return eventTimestamp;
}

// Retrieve an event from ChronoLog using a timestamp
std::vector <std::string> ChronoLogClient::retrieveEvents(std::uint64_t timestamp)
{
    if(storyHandle == nullptr)
    {
        return {};
    }
    // Replay events using the persistent story handle
    std::vector <std::string> matchingEvents = storyHandle->replay(timestamp);
    return matchingEvents;
}

} // namespace chronolog
