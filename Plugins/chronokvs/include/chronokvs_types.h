#ifndef CHRONOKVS_TYPES_H_
#define CHRONOKVS_TYPES_H_

#include <string>
#include <cstdint>
#include <utility>

namespace chronokvs {

struct EventData {
    std::uint64_t timestamp;  ///< The timestamp when the event was recorded
    std::string value;        ///< The stored value/content of the event

    EventData(std::uint64_t ts, std::string val) 
        : timestamp(ts)
        , value(std::move(val)) 
    {}

    EventData(const std::pair<std::uint64_t, std::string>& pair)
        : timestamp(pair.first)
        , value(pair.second) 
    {}
};

} // namespace chronokvs

#endif // CHRONOKVS_TYPES_H_
