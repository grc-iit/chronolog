#ifndef CHRONOPUBSUB_H_
#define CHRONOPUBSUB_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "chronopubsub_logger.h"
#include "chronopubsub_types.h"

namespace chronopubsub
{

class ChronoPubSubMapper;

/**
 * @brief Simple pub/sub facade over a single ChronoLog chronicle.
 *
 * Each topic maps to one story inside a default chronicle. Publishing a message
 * appends a ChronoLog event to the topic's story. Subscribers run a background
 * thread that polls the topic via ReplayStory from a per-subscription watermark
 * and dispatches every new event to a user callback in arrival order.
 *
 * This is a client-side wrapper aimed at extensibility, not throughput; the
 * polling cadence is configurable per subscription.
 */
class ChronoPubSub
{
public:
    using MessageCallback = std::function<void(const Message&)>;

private:
    std::unique_ptr<ChronoPubSubMapper> mapper;
    LogLevel logLevel_;

    explicit ChronoPubSub(LogLevel level);
    explicit ChronoPubSub(const std::string& config_path, LogLevel level);

public:
    /**
     * @brief Create a ChronoPubSub instance using built-in default ChronoLog
     *        client configuration (localhost deployment).
     *
     * Failures during configuration loading or ChronoLog connection are logged
     * at @p level and signalled by returning nullptr.
     */
    static std::unique_ptr<ChronoPubSub> Create(LogLevel level = getDefaultLogLevel()) noexcept;

    /**
     * @brief Create a ChronoPubSub instance loading a ChronoLog client config.
     *
     * Pass an empty string to fall back to the built-in defaults.
     */
    static std::unique_ptr<ChronoPubSub> Create(const std::string& config_path,
                                                LogLevel level = getDefaultLogLevel()) noexcept;

    ~ChronoPubSub();

    LogLevel getLogLevel() const { return logLevel_; }

    /**
     * @brief Publish a message to @p topic.
     * @return The ChronoLog event timestamp assigned to the message, or 0 on failure.
     */
    std::uint64_t publish(const std::string& topic, const std::string& payload);

    /**
     * @brief Subscribe to @p topic, receiving every message published from now on.
     *
     * The callback is invoked from a background polling thread. Subsequent
     * subscriptions to the same topic each get their own thread and watermark.
     *
     * @param topic                Topic name (story).
     * @param callback             Invoked once per delivered message.
     * @param poll_interval_ms     Polling cadence; default 100 ms.
     * @return SubscriptionId, or kInvalidSubscriptionId on failure.
     */
    SubscriptionId subscribe(const std::string& topic, MessageCallback callback, std::uint32_t poll_interval_ms = 100);

    /**
     * @brief Subscribe to @p topic, replaying every message with timestamp >= @p since_timestamp.
     */
    SubscriptionId subscribe_from(const std::string& topic,
                                  std::uint64_t since_timestamp,
                                  MessageCallback callback,
                                  std::uint32_t poll_interval_ms = 100);

    /**
     * @brief Stop and remove a subscription. Safe to call from outside the callback.
     * @return true if the id matched an active subscription.
     */
    bool unsubscribe(SubscriptionId id);

    /**
     * @brief Release any cached publish handles to commit pending writes for
     *        propagation. Subscribers on other processes must call this on the
     *        publisher before they can replay.
     */
    void flush();
};

} // namespace chronopubsub

#endif // CHRONOPUBSUB_H_
