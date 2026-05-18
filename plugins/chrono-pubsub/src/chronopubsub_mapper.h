#ifndef CHRONOPUBSUB_MAPPER_H_
#define CHRONOPUBSUB_MAPPER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "chronopubsub_client_adapter.h"
#include "chronopubsub_logger.h"
#include "chronopubsub_types.h"

namespace chronopubsub
{

class ChronoPubSubMapper
{
public:
    using MessageCallback = std::function<void(const Message&)>;

private:
    struct Subscription
    {
        SubscriptionId id;
        std::string topic;
        MessageCallback callback;
        std::uint32_t poll_interval_ms;
        std::atomic<bool> stop_requested;
        std::atomic<std::uint64_t> watermark; // next event timestamp to deliver
        std::thread worker;

        Subscription(SubscriptionId i, std::string t, MessageCallback cb, std::uint32_t poll_ms, std::uint64_t wm)
            : id(i)
            , topic(std::move(t))
            , callback(std::move(cb))
            , poll_interval_ms(poll_ms)
            , stop_requested(false)
            , watermark(wm)
        {}
    };

    std::unique_ptr<ChronoPubSubClientAdapter> chronoClientAdapter;
    LogLevel logLevel_;

    std::mutex subscriptionsMutex;
    std::unordered_map<SubscriptionId, std::shared_ptr<Subscription>> subscriptions;
    std::atomic<SubscriptionId> nextSubscriptionId{1};

    void runSubscription(std::shared_ptr<Subscription> sub);

public:
    explicit ChronoPubSubMapper(LogLevel level);
    explicit ChronoPubSubMapper(const std::string& config_path, LogLevel level);

    ~ChronoPubSubMapper();

    std::uint64_t publish(const std::string& topic, const std::string& payload);

    SubscriptionId subscribe(const std::string& topic,
                             std::uint64_t since_timestamp,
                             MessageCallback callback,
                             std::uint32_t poll_interval_ms);

    bool unsubscribe(SubscriptionId id);

    std::uint64_t now_timestamp() const;

    void flush();
};

} // namespace chronopubsub

#endif // CHRONOPUBSUB_MAPPER_H_
