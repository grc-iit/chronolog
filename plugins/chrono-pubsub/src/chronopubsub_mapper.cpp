#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "chronopubsub_mapper.h"

namespace chronopubsub
{

constexpr std::uint64_t MAX_TIMESTAMP = UINT64_MAX;

ChronoPubSubMapper::ChronoPubSubMapper(LogLevel level)
    : logLevel_(level)
{
    chronoClientAdapter = std::make_unique<ChronoPubSubClientAdapter>(level);
}

ChronoPubSubMapper::ChronoPubSubMapper(const std::string& config_path, LogLevel level)
    : logLevel_(level)
{
    chronoClientAdapter = std::make_unique<ChronoPubSubClientAdapter>(config_path, level);
}

ChronoPubSubMapper::~ChronoPubSubMapper()
{
    // Snapshot subscriptions under lock, then stop them outside the lock so a
    // worker thread can never block trying to take subscriptionsMutex.
    std::vector<std::shared_ptr<Subscription>> to_stop;
    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex);
        to_stop.reserve(subscriptions.size());
        for(auto& [id, sub]: subscriptions)
        {
            sub->stop_requested = true;
            to_stop.push_back(sub);
        }
        subscriptions.clear();
    }
    for(auto& sub: to_stop)
    {
        if(sub->worker.joinable())
        {
            sub->worker.join();
        }
    }
}

std::uint64_t ChronoPubSubMapper::publish(const std::string& topic, const std::string& payload)
{
    if(topic.empty())
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Invalid input: topic cannot be empty");
        throw std::invalid_argument("Topic cannot be empty");
    }
    if(payload.empty())
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Invalid input: payload cannot be empty for topic='", topic, "'");
        throw std::invalid_argument("Payload cannot be empty");
    }

    return chronoClientAdapter->publishEvent(topic, payload);
}

std::uint64_t ChronoPubSubMapper::now_timestamp() const
{
    // ChronoLog event timestamps come from the system clock in nanoseconds.
    // Using the same clock here gives a watermark that excludes earlier events.
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
}

SubscriptionId ChronoPubSubMapper::subscribe(const std::string& topic,
                                             std::uint64_t since_timestamp,
                                             MessageCallback callback,
                                             std::uint32_t poll_interval_ms)
{
    if(topic.empty())
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Invalid input: topic cannot be empty");
        throw std::invalid_argument("Topic cannot be empty");
    }
    if(!callback)
    {
        CHRONOPUBSUB_ERROR(logLevel_, "Invalid input: callback cannot be null for topic='", topic, "'");
        throw std::invalid_argument("Callback cannot be null");
    }

    SubscriptionId id = nextSubscriptionId.fetch_add(1);
    auto sub = std::make_shared<Subscription>(id, topic, std::move(callback), poll_interval_ms, since_timestamp);

    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex);
        subscriptions.emplace(id, sub);
    }

    sub->worker = std::thread([this, sub]() { runSubscription(sub); });

    CHRONOPUBSUB_INFO(logLevel_,
                      "Subscribed to topic='",
                      topic,
                      "' id=",
                      id,
                      " since_ts=",
                      since_timestamp,
                      " poll_ms=",
                      poll_interval_ms);
    return id;
}

bool ChronoPubSubMapper::unsubscribe(SubscriptionId id)
{
    std::shared_ptr<Subscription> sub;
    {
        std::lock_guard<std::mutex> lock(subscriptionsMutex);
        auto it = subscriptions.find(id);
        if(it == subscriptions.end())
        {
            return false;
        }
        sub = it->second;
        sub->stop_requested = true;
        subscriptions.erase(it);
    }

    // Don't join when called from inside the worker thread (callback-initiated
    // unsubscribe). Detach instead so the thread cleans up after itself.
    if(sub->worker.joinable())
    {
        if(sub->worker.get_id() == std::this_thread::get_id())
        {
            sub->worker.detach();
        }
        else
        {
            sub->worker.join();
        }
    }

    CHRONOPUBSUB_INFO(logLevel_, "Unsubscribed id=", id);
    return true;
}

void ChronoPubSubMapper::runSubscription(std::shared_ptr<Subscription> sub)
{
    while(!sub->stop_requested.load())
    {
        std::vector<Message> messages;
        try
        {
            messages = chronoClientAdapter->replayEvents(sub->topic, sub->watermark.load(), MAX_TIMESTAMP);
        }
        catch(const std::exception& e)
        {
            // Expected during the first ~120s after a publish: ChronoLog returns
            // CL_ERR_QUERY_TIMED_OUT until events have propagated to a player.
            // Log at WARNING so a hot polling loop does not flood stderr.
            CHRONOPUBSUB_WARNING(logLevel_,
                                 "Replay failed for topic='",
                                 sub->topic,
                                 "' id=",
                                 sub->id,
                                 " (will retry): ",
                                 e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(sub->poll_interval_ms));
            continue;
        }

        if(!messages.empty())
        {
            // ReplayStory does not guarantee ordering; sort by timestamp so
            // subscribers see events in arrival order.
            std::sort(messages.begin(),
                      messages.end(),
                      [](const Message& a, const Message& b) { return a.timestamp < b.timestamp; });

            std::uint64_t max_seen = sub->watermark.load();
            for(const auto& msg: messages)
            {
                if(sub->stop_requested.load())
                {
                    break;
                }
                if(msg.timestamp < sub->watermark.load())
                {
                    continue; // safety: skip anything before our watermark
                }
                try
                {
                    sub->callback(msg);
                }
                catch(const std::exception& e)
                {
                    CHRONOPUBSUB_ERROR(logLevel_,
                                       "Subscriber callback threw for topic='",
                                       sub->topic,
                                       "' id=",
                                       sub->id,
                                       ": ",
                                       e.what());
                }
                catch(...)
                {
                    CHRONOPUBSUB_ERROR(logLevel_,
                                       "Subscriber callback threw unknown exception for topic='",
                                       sub->topic,
                                       "' id=",
                                       sub->id);
                }
                if(msg.timestamp > max_seen)
                {
                    max_seen = msg.timestamp;
                }
            }
            // Advance watermark past the last delivered timestamp.
            if(max_seen != UINT64_MAX)
            {
                sub->watermark.store(max_seen + 1);
            }
        }

        if(!sub->stop_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sub->poll_interval_ms));
        }
    }
}

void ChronoPubSubMapper::flush() { chronoClientAdapter->flush(); }

} // namespace chronopubsub
