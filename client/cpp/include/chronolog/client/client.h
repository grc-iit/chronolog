#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include "chronolog/client/clock.h"
#include "chronolog/predicate.h"
#include "chronolog/types.h"

namespace chronolog::client
{
using Deadline = std::optional<std::chrono::system_clock::time_point>;
using AppendRejection = chronolog::AppendRejection;
AppendRejection rejectionOf(const absl::Status&);
using AcquireOptions = chronolog::AcquireOptions;
using AcquireRefusal = chronolog::AcquireRefusal;
// Typed HELD, PRIOR_MISMATCH or terminal-retry detail of a refused acquire; never parsed from the message.
std::optional<AcquireRefusal> acquireRefusalOf(const absl::Status&);
enum class AwaitAnswer
{
    Visible = 1,
    Absent = 2,
    SequenceConsumed = 3,
    WillNeverExist = 4,
    Unknown = 5
};
struct EventRef
{
    EventId id;
    std::optional<Hlc> hlc;
};
struct AwaitResult
{
    AwaitAnswer answer{AwaitAnswer::Unknown};
    std::optional<Event> event;
    std::optional<Hlc> frontier;
};
struct RetryPolicy
{
    size_t max_retries{3};
    std::chrono::milliseconds backoff{20};
};
struct LeaseOptions
{
    // Renewal starts once the local estimate of the last Catalog-confirmed grant is within
    // lead_fraction of its duration plus margin of expiring.
    double lead_fraction{0.5};
    std::chrono::milliseconds margin{2000};
    // Longest single scheduler sleep, which bounds how late renewal starts after a client suspend.
    std::chrono::milliseconds poll{1000};
    size_t max_batch{256};
    // Local CLOCK_BOOTTIME in nanoseconds; empty reads the system clock. Tests inject one.
    std::function<int64_t()> boottime_ns;
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
    LeaseOptions lease;
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
struct ReadOptions
{
    std::optional<uint32_t> max_events;
    // I6.17: only matching events are returned and the range, order and completeness are unchanged. An empty predicate
    // matches every event. The Player validates it.
    EventPredicate predicate{};
};
struct TailOptions
{
    // I6.17: only matching events are delivered and the delivery frontier is unchanged.
    EventPredicate predicate{};
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
struct WriterLease
{
    // Last Catalog-confirmed grant; Keeper admissions never refresh it.
    AcquisitionLease grant;
    int64_t estimated_remaining_ns{};
    // Diagnostic only: false after a missed horizon or failed renewal. Dispatch never stops on it.
    bool confirmed{true};
    std::optional<AcquisitionTerminationCause> termination_cause;
    uint64_t renewals{};
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
    // Stops renewal before sending Release.
    absl::StatusOr<bool> release(Deadline deadline = {});
    WriterLease lease() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Writer(std::unique_ptr<Impl>);
    friend class Client;
    friend class LaneWriter;
};
// One logical writer spread across Keepers by time. Each lane is an ordinary Writer named identity + "/lane" + i,
// so sequence, incarnation and fencing semantics hold per lane. A spec without a physical reading is stamped with
// one Client clock reading before its first send, and a retry of the same spec keeps that stamp and so its lane.
class LaneWriter
{
public:
    ~LaneWriter();
    LaneWriter(LaneWriter&&) noexcept;
    LaneWriter& operator=(LaneWriter&&) noexcept;
    LaneWriter(const LaneWriter&) = delete;
    LaneWriter& operator=(const LaneWriter&) = delete;
    size_t lanes() const;
    absl::StatusOr<AppendResult> append(const AppendSpec&, Deadline deadline = {});
    // Splits by lane keeping each lane's input order; results come back in input order.
    absl::StatusOr<BatchResult> appendBatch(std::span<const AppendSpec>, Deadline deadline = {});
    // Releases every lane and returns the first error, or whether every lane reported a release.
    absl::StatusOr<bool> release(Deadline deadline = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit LaneWriter(std::unique_ptr<Impl>);
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
    // Raises this Client's shared causal floor without lowering it (I7.3).
    void observeFloor(Hlc floor);
    // Returns this Client's current shared causal floor (I7.3).
    Hlc causalFloor() const;
    absl::StatusOr<Chronicle> createChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<Chronicle> getChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<std::vector<Chronicle>> listChronicles(Deadline deadline = {});
    absl::Status destroyChronicle(const std::string&, Deadline deadline = {});
    absl::StatusOr<Story> createStory(const std::string& chronicle, const std::string& name, Deadline deadline = {});
    absl::StatusOr<Story> getStory(StoryId, Deadline deadline = {});
    absl::StatusOr<Route> route(StoryId, Deadline deadline = {});
    absl::StatusOr<std::vector<Story>> listStories(const std::string& chronicle, Deadline deadline = {});
    // I9.3: the stories whose path equals the prefix or lies below it by whole segments, never tombstoned.
    // RESOURCE_EXHAUSTED when more than 65536 match.
    absl::StatusOr<std::vector<Story>> listStoriesByPrefix(const std::string& prefix, Deadline deadline = {});
    absl::Status destroyStory(StoryId, Deadline deadline = {});
    absl::StatusOr<Writer> acquire(StoryId, const std::string& identity, Deadline deadline = {});
    // An empty acquire_request_id reuses this Client's unresolved id from an identical earlier call, else mints one.
    // A supplied id must come from newAcquireRequestId on this Client in this process. Without explicit takeover or
    // expected_prior_incarnation, an own unreleased or expired prior incarnation is recovered conditionally.
    absl::StatusOr<Writer> acquire(StoryId, const std::string& identity, AcquireOptions, Deadline deadline = {});
    // A random 128-bit id owned by this Client in this process, to retain one logical acquire across calls.
    absl::StatusOr<std::string> newAcquireRequestId();
    // Acquires min(lanes, route.keepers.size()) writers, lane i preferring route.keepers[i]. The lane of an append is
    // (physical_ns / slice_ns) % lanes. A caller-supplied acquire_request_id is refused because each lane mints its own.
    absl::StatusOr<LaneWriter> acquireLanes(StoryId,
                                            const std::string& identity,
                                            size_t lanes,
                                            int64_t slice_ns,
                                            AcquireOptions = {},
                                            Deadline deadline = {});
    absl::StatusOr<AwaitResult> await(EventRef ref, std::chrono::nanoseconds bound, Deadline deadline = {});
    absl::StatusOr<ReadStream> read(StoryId, HlcRange, Deadline deadline = {});
    absl::StatusOr<ReadStream> read(StoryId, HlcRange, ReadOptions, Deadline deadline = {});
    absl::StatusOr<ReadStream> readPhysical(StoryId, PhysicalRange, Deadline deadline = {});
    absl::StatusOr<TailStream> tail(StoryId, std::optional<Position> after = {}, Deadline deadline = {});
    absl::StatusOr<TailStream> tail(StoryId, std::optional<Position> after, TailOptions, Deadline deadline = {});
    // I6.15: the merged history of every story whose path equals the prefix or lies below it by whole segments, in
    // the order of I7.7. The Completion names the Catalog revision it resolved the set at.
    absl::StatusOr<ReadStream> read(const std::string& prefix, HlcRange, Deadline deadline = {});
    absl::StatusOr<ReadStream> read(const std::string& prefix, HlcRange, ReadOptions, Deadline deadline = {});
    // The story set is fixed when the stream opens; a story created later joins at the next subscription.
    absl::StatusOr<TailStream>
    tail(const std::string& prefix, std::optional<Position> after = {}, Deadline deadline = {});
    absl::StatusOr<TailStream>
    tail(const std::string& prefix, std::optional<Position> after, TailOptions, Deadline deadline = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Client(std::unique_ptr<Impl>);
};
} // namespace chronolog::client
