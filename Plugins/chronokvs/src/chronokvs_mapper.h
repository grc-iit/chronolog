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

namespace chronolog
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
    // Default constructor that initializes MemoryManager and ChronoLogClient
    chronokvs_mapper(): memoryManager(std::make_unique <MemoryManager>()), chronoClient(
            std::make_unique <ChronoLogClient>())
    {}

    std::uint64_t storeKeyValue(const std::string &key, const std::string &value);

    std::vector <std::pair <std::string, std::string>> retrieveByTimestamp(std::uint64_t timestamp);

    std::vector <std::pair <std::uint64_t, std::string>> retrieveByKey(const std::string &key);

    std::vector <std::string> retrieveByKeyAndTimestamp(const std::string &key, std::uint64_t timestamp);

}; // namespace chronolog
}
#endif // KVS_MAPPER_H_