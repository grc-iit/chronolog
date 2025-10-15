#ifndef CHRONOKVS_H_
#define CHRONOKVS_H_

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "chronokvs_types.h"

namespace chronokvs
{

// Forward declaration of the private mapper type (header lives in src/)
class ChronoKVSMapper;

class ChronoKVS
{
private:
    std::unique_ptr<ChronoKVSMapper> mapper;

public:

    ChronoKVS();

    ~ChronoKVS();

    std::uint64_t put(const std::string &key, const std::string &value);

    std::string get(const std::string &key, uint64_t timestamp);

    std::vector<EventData> get_history(const std::string &key);
};

}

#endif // CHRONOKVS_H_