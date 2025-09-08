#ifndef KVS_MAPPER_H_
#define KVS_MAPPER_H_

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <optional>
#include <unordered_map>
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

    std::uint64_t storeKeyValue(const std::string &key, const std::string &value);

    std::vector <std::pair <std::uint64_t, std::string>> retrieveByKey(const std::string &key);

    std::string retrieveByKeyAndTs(const std::string &key, std::uint64_t timestamp);

};
}
#endif // KVS_MAPPER_H_