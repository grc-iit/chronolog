#ifndef CHRONOPUBSUB_TYPES_H_
#define CHRONOPUBSUB_TYPES_H_

#include <cstdint>
#include <string>
#include <utility>

namespace chronopubsub
{

struct Message
{
    std::uint64_t timestamp;
    std::string topic;
    std::string payload;

    Message(std::uint64_t ts, std::string t, std::string p)
        : timestamp(ts)
        , topic(std::move(t))
        , payload(std::move(p))
    {}
};

using SubscriptionId = std::uint64_t;

constexpr SubscriptionId kInvalidSubscriptionId = 0;

} // namespace chronopubsub

#endif // CHRONOPUBSUB_TYPES_H_
