#ifndef CHRONOKVS_CLIENT_ADAPTER_H_
#define CHRONOKVS_CLIENT_ADAPTER_H_

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include <chronolog_client.h>

namespace chronokvs
{

class ChronoKVSClientAdapter
{
private:
    std::unique_ptr<chronolog::Client> chronolog;
    std::unique_ptr<chronolog::StoryHandle> storyHandle;
    const std::string defaultChronicle = "ChronoKVSChronicle";
    const std::string defaultStory = "ChronoKVSStore";

public:
    ChronoKVSClientAdapter();

    ~ChronoKVSClientAdapter();

    std::uint64_t storeEvent(const std::string &serializedEvent);

    std::vector<std::string> retrieveEvents(std::uint64_t timestamp);
};
}
#endif // CHRONOKVS_CLIENT_ADAPTER_H_