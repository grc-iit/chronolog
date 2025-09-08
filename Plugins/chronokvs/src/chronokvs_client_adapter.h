#ifndef CHRONOKVS_CLIENT_ADAPTER_H_
#define CHRONOKVS_CLIENT_ADAPTER_H_

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include "chronolog_client.h"

namespace chronokvs
{

class ChronoKVSClientAdapter
{
private:
    std::shared_ptr<chronoemulator::ChronoEmulator> chronolog;
    chronoemulator::StoryHandle* storyHandle;

public:
    ChronoKVSClientAdapter();

    ~ChronoKVSClientAdapter();

    std::uint64_t storeEvent(const std::string &serializedEvent);

    std::vector <std::string> retrieveEvents(std::uint64_t timestamp);

    std::vector <std::string> retrieveEvents(std::uint64_t start_timestamp, std::uint64_t end_timestamp);
};
}
#endif // CHRONOKVS_CLIENT_ADAPTER_H_