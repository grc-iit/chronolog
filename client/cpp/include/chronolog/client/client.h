#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include "chronolog/client/clock.h"
#include "chronolog/types.h"

namespace chronolog::client
{
using Deadline = std::optional<std::chrono::system_clock::time_point>;
struct RetryPolicy
{
    size_t max_retries{3};
    std::chrono::milliseconds backoff{20};
};
struct ClientOptions
{
    // One host:port or a comma-separated list of IPv4 literals or host names; explicit gRPC targets also work.
    std::string catalog_endpoint;
    std::string player_endpoint;
    std::chrono::milliseconds rpc_timeout{10000};
    RetryPolicy retry;
    std::map<std::string, std::variant<int, std::string>> channel_args;
    TimeSource time_source;
    size_t max_in_flight{4};
    size_t batch_size{128};
    size_t max_batch_items{10000};
    size_t max_batch_bytes{64u << 20};
};
struct AppendSpec
{
    Envelope envelope;
    Durability durability{Durability::Durable};
    std::optional<TimeReading> physical{};
};
struct AppendResult
{
    EventId event_id;
    Hlc hlc;
    Durability achieved{Durability::Unspecified};
    bool acked() const { return achieved == Durability::Durable; }
};
using BatchResult = std::vector<absl::StatusOr<AppendResult>>;
struct HlcRange
{
    Hlc start;
    Hlc end;
};
struct PhysicalRange
{
    int64_t start_ns;
    int64_t end_ns;
};
struct Position
{
    Hlc hlc;
    EventId id;
};
struct StreamItem
{
    std::vector<Event> events;
    std::optional<Completion> completion;
    // Inclusive start of the next HLC Read after a TRUNCATED completion.
    std::optional<Hlc> continuation;
};
class ReadStream
{
public:
    ~ReadStream();
    ReadStream(ReadStream&&) noexcept;
    ReadStream& operator=(ReadStream&&) noexcept;
    ReadStream(const ReadStream&) = delete;
    ReadStream& operator=(const ReadStream&) = delete;
    absl::StatusOr<std::optional<StreamItem>> next(Deadline deadline = {});
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ReadStream(std::unique_ptr<Impl>);
    friend class Client;
};
class TailStream
{
public:
    ~TailStream();
    TailStream(TailStream&&) noexcept;
    TailStream& operator=(TailStream&&) noexcept;
    TailStream(const TailStream&) = delete;
    TailStream& operator=(const TailStream&) = delete;
    absl::StatusOr<std::optional<StreamItem>> next(Deadline deadline = {});
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit TailStream(std::unique_ptr<Impl>);
    friend class Client;
};
class Writer
{
public:
    ~Writer();
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    Acquisition acquisition() const;
    absl::StatusOr<AppendResult> append(const AppendSpec&, Deadline deadline = {});
    absl::StatusOr<BatchResult> appendBatch(std::span<const AppendSpec>, Deadline deadline = {});
    absl::StatusOr<bool> release(Deadline deadline = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Writer(std::unique_ptr<Impl>);
    friend class Client;
};
class Client
{
public:
    static absl::StatusOr<Client> Connect(ClientOptions, Deadline deadline = {});
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    absl::StatusOr<Chronicle> createChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<Chronicle> getChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<std::vector<Chronicle>> listChronicles(Deadline deadline = {});
    absl::Status destroyChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<Story> createStory(const std::string& chronicle, const std::string& name, Deadline deadline = {});
    absl::StatusOr<Story> getStory(StoryId, Deadline deadline = {});
    absl::StatusOr<std::vector<Story>> listStories(const std::string& chronicle, Deadline deadline = {});
    absl::Status destroyStory(StoryId, Deadline deadline = {});
    absl::StatusOr<Writer> acquire(StoryId, const std::string& identity, Deadline deadline = {});
    absl::StatusOr<ReadStream> read(StoryId, HlcRange, Deadline deadline = {});
    absl::StatusOr<ReadStream> readPhysical(StoryId, PhysicalRange, Deadline deadline = {});
    absl::StatusOr<TailStream> tail(StoryId, std::optional<Position> after = {}, Deadline deadline = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Client(std::unique_ptr<Impl>);
};
} // namespace chronolog::client
