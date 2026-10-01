#pragma once
#include "chronolog/client/client.h"
#include <list>
#include <unordered_map>

namespace chronolog::kvs
{
struct Version
{
    EventId event_id;
    Hlc hlc;
    Durability durability{Durability::Unspecified};
    bool acknowledged{};
    bool acked() const { return acknowledged; }
};
struct Value
{
    std::string value;
    Version version;
    bool deleted{};
    Envelope envelope;
};
struct GetResult
{
    absl::Status status;
    std::optional<Value> value;
    Completion completion;
};
struct PutOptions
{
    Envelope metadata;
    Durability durability{Durability::Durable};
    client::Deadline deadline{};
};
struct GetOptions
{
    std::optional<Hlc> at{};
    std::optional<Hlc> causal_floor{};
    client::Deadline deadline{};
};
struct StoreOptions
{
    size_t max_keys{64};
    size_t max_versions_per_key{64};
    size_t max_bytes_per_key{1u << 20};
};
struct HistoryItem
{
    std::vector<Value> versions;
    std::optional<Completion> completion;
    std::optional<Hlc> continuation;
};
class History
{
public:
    explicit History(client::ReadStream stream)
        : stream_(std::move(stream))
    {}
    absl::StatusOr<std::optional<HistoryItem>> next(client::Deadline deadline = {});
    void cancel() { stream_.cancel(); }

private:
    client::ReadStream stream_;
};
// The Client outlives the Store; calls on a Store are serialized by its caller.
class Store
{
public:
    Store(client::Client& client, std::string chronicle, StoreOptions options = {});
    absl::StatusOr<Version> put(const std::string& key, std::string value, PutOptions options = {});
    absl::StatusOr<std::vector<absl::StatusOr<Version>>>
    putBatch(const std::string& key, std::span<const std::string> values, PutOptions options = {});
    absl::StatusOr<Version> erase(const std::string& key, PutOptions options = {});
    absl::StatusOr<GetResult> get(const std::string& key, GetOptions options = {});
    absl::StatusOr<GetResult> get(const std::string& key, Hlc at, GetOptions options = {})
    {
        options.at = at;
        return get(key, std::move(options));
    }
    absl::StatusOr<History> history(const std::string& key, client::HlcRange range, client::Deadline deadline = {});
    absl::Status watch(const std::string& key, client::Deadline deadline = {});
    absl::StatusOr<HistoryItem> follow(const std::string& key, client::Deadline deadline = {});
    size_t cachedKeys() const { return entries_.size(); }

private:
    struct Entry
    {
        StoryId story{};
        std::vector<Event> events;
        Hlc floor{}, end{};
        Completion completion;
        std::optional<client::TailStream> tail;
        std::list<std::string>::iterator lru;
    };
    absl::StatusOr<Entry*> entry(const std::string& key, bool create, client::Deadline deadline);
    void insert(Entry& entry, Event event);
    absl::StatusOr<Version> append(const std::string& key, std::string value, bool deleted, PutOptions options);
    client::Client& client_;
    std::string chronicle_, identity_;
    StoreOptions options_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
};
} // namespace chronolog::kvs
