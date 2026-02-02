#ifndef CHRONOKVS_CLIENT_ADAPTER_H_
#define CHRONOKVS_CLIENT_ADAPTER_H_

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>

#include <chronolog_client.h>

#include "chronokvs_types.h"
#include "chronokvs_logger.h"

namespace chronokvs
{

class ChronoKVSClientAdapter
{
private:
    std::unique_ptr<chronolog::Client> chronolog;
    const std::string defaultChronicle = "ChronoKVSChronicle";
    LogLevel logLevel_;

public:
    explicit ChronoKVSClientAdapter(LogLevel level);

    ~ChronoKVSClientAdapter();

    std::uint64_t storeEvent(const std::string& key, const std::string& value);

    std::vector<EventData> retrieveEvents(const std::string& key, std::uint64_t start_ts, std::uint64_t end_ts);
};
} // namespace chronokvs
#endif // CHRONOKVS_CLIENT_ADAPTER_H_