#ifndef KVS_MAPPER_H_
#define KVS_MAPPER_H_

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>

#include "chronokvs_types.h"
#include "chronokvs_client_adapter.h"

namespace chronokvs
{

class ChronoKVSMapper
{
private:
    std::unique_ptr<ChronoKVSClientAdapter> chronoClientAdapter;

public:
    ChronoKVSMapper();

    ~ChronoKVSMapper() = default;

    std::uint64_t storeKeyValue(const std::string& key, const std::string& value);

    std::string retrieveByKeyAndTs(const std::string& key, std::uint64_t timestamp);

    std::vector<EventData> retrieveByKey(const std::string& key);

    std::vector<EventData>
    retrieveByKeyAndRange(const std::string& key, std::uint64_t start_timestamp, std::uint64_t end_timestamp);

    std::optional<EventData> retrieveEarliestByKey(const std::string& key);

    std::optional<EventData> retrieveLatestByKey(const std::string& key);
};
} // namespace chronokvs
#endif // KVS_MAPPER_H_
