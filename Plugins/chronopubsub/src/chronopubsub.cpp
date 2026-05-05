#include <cstdint>
#include <memory>
#include <string>

#include "chronopubsub.h"
#include "chronopubsub_mapper.h"

namespace chronopubsub
{

ChronoPubSub::ChronoPubSub(LogLevel level)
    : mapper(std::make_unique<ChronoPubSubMapper>(level))
    , logLevel_(level)
{}

ChronoPubSub::ChronoPubSub(const std::string& config_path, LogLevel level)
    : mapper(std::make_unique<ChronoPubSubMapper>(config_path, level))
    , logLevel_(level)
{}

std::unique_ptr<ChronoPubSub> ChronoPubSub::Create(LogLevel level) noexcept
{
    try
    {
        return std::unique_ptr<ChronoPubSub>(new ChronoPubSub(level));
    }
    catch(const std::exception& e)
    {
        CHRONOPUBSUB_ERROR(level, "ChronoPubSub construction failed: ", e.what());
        return nullptr;
    }
    catch(...)
    {
        CHRONOPUBSUB_ERROR(level, "ChronoPubSub construction failed: unknown exception");
        return nullptr;
    }
}

std::unique_ptr<ChronoPubSub> ChronoPubSub::Create(const std::string& config_path, LogLevel level) noexcept
{
    try
    {
        return std::unique_ptr<ChronoPubSub>(new ChronoPubSub(config_path, level));
    }
    catch(const std::exception& e)
    {
        CHRONOPUBSUB_ERROR(level, "ChronoPubSub construction failed (config_path='", config_path, "'): ", e.what());
        return nullptr;
    }
    catch(...)
    {
        CHRONOPUBSUB_ERROR(level,
                           "ChronoPubSub construction failed (config_path='",
                           config_path,
                           "'): unknown exception");
        return nullptr;
    }
}

ChronoPubSub::~ChronoPubSub() = default;

std::uint64_t ChronoPubSub::publish(const std::string& topic, const std::string& payload)
{
    CHRONOPUBSUB_DEBUG(logLevel_, "publish() topic='", topic, "' payload_size=", payload.size());
    return mapper->publish(topic, payload);
}

SubscriptionId
ChronoPubSub::subscribe(const std::string& topic, MessageCallback callback, std::uint32_t poll_interval_ms)
{
    CHRONOPUBSUB_DEBUG(logLevel_, "subscribe() topic='", topic, "'");
    return mapper->subscribe(topic, mapper->now_timestamp(), std::move(callback), poll_interval_ms);
}

SubscriptionId ChronoPubSub::subscribe_from(const std::string& topic,
                                            std::uint64_t since_timestamp,
                                            MessageCallback callback,
                                            std::uint32_t poll_interval_ms)
{
    CHRONOPUBSUB_DEBUG(logLevel_, "subscribe_from() topic='", topic, "' since=", since_timestamp);
    return mapper->subscribe(topic, since_timestamp, std::move(callback), poll_interval_ms);
}

bool ChronoPubSub::unsubscribe(SubscriptionId id)
{
    CHRONOPUBSUB_DEBUG(logLevel_, "unsubscribe() id=", id);
    return mapper->unsubscribe(id);
}

void ChronoPubSub::flush()
{
    CHRONOPUBSUB_DEBUG(logLevel_, "flush() called");
    mapper->flush();
}

} // namespace chronopubsub
