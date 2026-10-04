#pragma once
#include "chronolog/kvs/store.h"
#include <functional>
#include <mutex>
#include <thread>

namespace chronolog::pubsub
{
struct Message
{
    std::string topic;
    Event event;
    client::Position position() const { return {event.hlc, event.id}; }
};
using Callback = std::function<absl::Status(const Message&)>;
enum class Start
{
    Earliest,
    Latest,
    Saved
};
struct SubscribeOptions
{
    Start start{Start::Latest};
    std::optional<client::Position> position{};
    client::Deadline deadline{};
    std::chrono::milliseconds pull_timeout{250};
    std::chrono::milliseconds retry_delay{50};
};
struct PublishOptions
{
    Envelope metadata;
    Durability durability{Durability::Durable};
    client::Deadline deadline{};
};
class Subscription
{
public:
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    void requestStop();
    void stop();
    std::optional<client::Position> position() const;
    absl::Status status() const;
    std::optional<Completion> completion() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
    explicit Subscription(std::shared_ptr<State> state);
    friend class Bus;
};
// Client outlives Bus and its subscriptions. Callbacks must return promptly;
// OK commits the cursor, while an error retries the same EventId.
class Bus
{
public:
    Bus(client::Client& client, std::string chronicle);
    absl::StatusOr<client::AppendResult>
    publish(const std::string& topic, std::string payload, PublishOptions options = {});
    absl::StatusOr<std::unique_ptr<Subscription>>
    subscribe(const std::string& topic, Callback callback, SubscribeOptions options = {});

private:
    absl::StatusOr<StoryId> topic(const std::string& name, client::Deadline deadline);
    client::Client& client_;
    std::string chronicle_, identity_;
    std::timed_mutex publish_mutex_;
};
absl::StatusOr<kvs::Version>
savePosition(kvs::Store& store, const std::string& key, client::Position position, client::Deadline deadline = {});
absl::StatusOr<client::Position> loadPosition(kvs::Store& store, const std::string& key, kvs::GetOptions options = {});
} // namespace chronolog::pubsub
