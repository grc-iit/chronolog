#ifndef KVS_MAPPER_H_
#define KVS_MAPPER_H_

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <optional>
#include <unordered_map>
#include "chronolog_client.h"
#include "chronokvs_memorymanager.h"

namespace chronokvs
{
/**
 * Handles mapping between key-value pairs and ChronoLog's data model.
 */
class chronokvs_mapper
{
private:
    std::unique_ptr <MemoryManager> memoryManager;
    std::unique_ptr <ChronoLogClient> chronoClient;

public:
    chronokvs_mapper();

    ~chronokvs_mapper() = default;

    std::uint64_t storeKeyValue(const std::string &key, const std::string &value);

    std::vector <std::pair <std::string, std::string>> retrieveByTimestamp(std::uint64_t timestamp);

    std::vector <std::pair <std::uint64_t, std::string>> retrieveByKey(const std::string &key);

    std::vector <std::string> retrieveByKeyAndTimestamp(const std::string &key, std::uint64_t timestamp);

};
}
#endif // KVS_MAPPER_H_