#pragma once
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
namespace chronolog
{
using StoryId = uint64_t;
// Ownership generation; epochs strictly increase, static until M8.
using Epoch = uint64_t;
// Identity is independent of timestamps. Sequence starts at one per incarnation.
struct EventId
{
    StoryId story_id{};
    // Catalog-assigned writer identity; zero is unassigned.
    uint64_t writer_id{};
    // Persisted generation; strictly increases when a writer re-acquires.
    uint64_t incarnation{};
    // Starts at 1; zero is not a valid accepted event sequence.
    uint64_t sequence{};
    auto operator<=>(const EventId&) const = default;
};
struct Hlc
{
    // CLOCK_REALTIME nanoseconds for readings; HLC physical component uses nanoseconds.
    int64_t physical_ns{};
    // Dimensionless HLC counter; overflow carries into physical_ns.
    uint32_t logical{};
    auto operator<=>(const Hlc&) const = default;
};
enum class ClockStatus
{
    Synced,
    Unsynced,
    Unavailable
};
// A missing bound means infinite/unknown uncertainty, never zero uncertainty.
struct TimeReading
{
    // CLOCK_REALTIME nanoseconds for readings; HLC physical component uses nanoseconds.
    int64_t physical_ns{};
    // Nanoseconds; absent means unbounded; finite only when status is Synced.
    std::optional<uint64_t> uncertainty_ns;
    ClockStatus status{ClockStatus::Unavailable};
};
struct PhysicalPolicy
{
    int64_t acceptance_window_ns{15'000'000'000};
    int64_t skew_limit_ns{60'000'000'000};
    int64_t hlc_lead_ns{61'000'000'000};
    uint64_t uncertainty_cap_ns{1'000'000'000};
    uint64_t version{1};
    auto operator<=>(const PhysicalPolicy&) const = default;
};
struct PhysicalInterval
{
    int64_t lo{}, hi{};
    bool bounded{};
};
struct CheckedAssignment
{
    Hlc hlc;
    int64_t acceptance_clock_ns{};
};
enum class Durability
{
    Unspecified = 0,
    Accepted = 1,
    Durable = 2
};
struct KeeperRef
{
    std::string process_id;
    std::string endpoint;
    auto operator<=>(const KeeperRef&) const = default;
};
struct Route
{
    // Ownership generation; zero denotes an uninitialized route.
    Epoch epoch{};
    std::vector<KeeperRef> keepers;
    std::string grapher;
    std::string player;
    auto operator<=>(const Route&) const = default;
};
struct Envelope
{
    std::string content_type;

    // Binary payload; configurable limit defaults to 1 MiB.
    std::string payload;

    // Optional binary W3C trace id: empty or exactly 16 bytes.
    std::string trace_id;

    // Optional binary W3C span id: empty or exactly 8 bytes.
    std::string span_id;
    std::map<std::string, std::string> attributes;
};
struct Event
{
    EventId id;
    TimeReading physical;
    Hlc hlc;
    Envelope envelope;
    // Achieved level: Accepted may vanish on crash; Durable is stable.
    Durability durability{Durability::Unspecified};
};
// Identity is completed with AppendBatch.story_id at acceptance.
struct AppendItem
{
    uint64_t writer_id{};
    uint64_t incarnation{};
    // Gapless sequence starting at 1.
    uint64_t sequence{};
    TimeReading physical;
    Hlc causal_floor;
    Envelope envelope;
};
// All items share a story and ownership epoch; mixed-story batches are impossible.
struct AppendBatch
{
    StoryId story_id{};
    Epoch epoch{};
    std::vector<AppendItem> items;
};
// Catalog release has committed when this value is returned successfully.
struct ReleaseResult
{
    // True only after assigned Keeper confirmed applying the release revision.
    // False means committed but unconfirmed after configured timeout.
    bool fenced{};
    uint64_t revision{};
};
struct KeeperFrontier
{
    // Endpoint identifies a Keeper in the current story Route.
    std::string keeper;
    Epoch epoch{};
    Hlc frontier;
    // False denotes a Keeper which did not answer; frontier is then not evidence.
    bool answered{true};
};
struct AppendResult
{
    absl::Status status;
    // Unspecified denotes no successful achieved level, never a Durable ack.
    Durability achieved{Durability::Unspecified};
    Hlc hlc;
    EventId id;
    // Present on stale-epoch failure; absent when no redirect is required.
    std::optional<Route> current_route;
};
struct Range
{
    // OPEN (M6): physical-axis completeness requires a policy bounding backdating.
    enum class Axis
    {
        Hlc,
        Physical
    };
    Axis axis{Axis::Hlc};
    // Inclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc start;
    // Exclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc end;
}; // [start,end); Physical ignores logical.
struct Predecessor
{
    KeeperRef keeper;
    std::string instance;
    Epoch epoch{};
    Hlc own_cut;
    int64_t own_physical_ceiling_ns{};
};
struct RouteState
{
    Route route;
    Hlc ordering_cut;
    int64_t physical_floor{};
    std::vector<Predecessor> predecessors;
    Hlc archived_below;
    std::vector<Range> abandoned;
};
// Keeper-sealed exclusive frontier: every event below F is visible, and no future
// event can be assigned below F. Shared by all writers on that Keeper, even idle
// writers. Pending DURABLE fsync prevents seal advancing beyond that event HLC.
struct Frontier
{
    // Catalog-assigned writer identity; zero is unassigned.
    uint64_t writer_id{};
    // Persisted generation; strictly increases when a writer re-acquires.
    uint64_t incarnation{};
    Hlc frontier;
};
enum class IncompleteReason
{
    None,
    LaggingWriters,
    PhysicalAxisUnbounded,
    SourceFailed,
    Truncated
};
struct Completion
{
    // Always false for physical-axis reads and tail subscriptions in v1.
    bool complete{};
    Hlc frontier;
    std::vector<Frontier> laggards;
    // None for complete HLC reads; physical reads use PhysicalAxisUnbounded.
    IncompleteReason reason{IncompleteReason::None};
};
struct Chronicle
{
    std::string name;
    bool tombstoned{};
};
struct Story
{
    StoryId id{};
    std::string chronicle;
    std::string name;
    Epoch epoch{};
    bool tombstoned{};
};
struct Acquisition
{
    StoryId story_id{};
    // Catalog-assigned writer identity; zero is unassigned.
    uint64_t writer_id{};
    // Persisted generation; strictly increases when a writer re-acquires.
    uint64_t incarnation{};
    Route route;
    // Single Keeper endpoint; stable per (writer_id, route.epoch).
    KeeperRef assigned_keeper;
};
enum class ProcessRole
{
    Keeper,
    Grapher,
    Player
};
struct Process
{
    std::string id;

    std::string instance;

    std::string endpoint;
    ProcessRole role{ProcessRole::Keeper};
};
struct Chunk
{
    std::string id;
    StoryId story_id{};
    // Inclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc start;
    // Exclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc end;
    std::vector<Event> events;
    // Salvage data remains readable but cannot advance contiguous watermark.
    bool exempt{};
    bool physical_policy{};
};
enum class ManifestState
{
    Published,
    Empty,
    Deleted,
    Failed,
    Lost
};
struct ManifestRecord
{
    std::string chunk_id;

    // Grapher instance owning this manifest log; distinct from event writer_id.
    std::string manifest_writer;

    std::string file;
    StoryId story_id{};
    // Inclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc start;
    // Exclusive HLC bound, or physical ns bound when Range::Axis is Physical.
    Hlc end;
    // Number of events, not bytes; zero for an empty window.
    uint64_t event_count{};
    ManifestState state{ManifestState::Published};
    // Salvage data remains readable but cannot advance contiguous watermark.
    bool exempt{};
    bool physical_policy{};
};
struct ChunkReceipt
{
    std::string chunk_id;

    std::string grapher_instance;
    // Transferred byte count, not an event count.
    uint64_t bytes{};
    // Zero denotes no tracked receipt; nonzero scoped to grapher_instance.
    uint64_t receipt{};
};
struct WatermarkReport
{
    StoryId story_id{};
    Hlc watermark;
    std::string grapher_instance;
    // Inclusive receipt number; zero means no receipts issued in this instance.
    uint64_t highest_receipt{};
    std::vector<uint64_t> pending_receipts;
    // True is a story tombstone signal, never a timestamp sentinel.
    bool dropped{};
};
struct ReplayBatch
{
    std::vector<Event> events;
    // Present only for a terminal stream message.
    std::optional<Completion> completion;
};
// Total order within one story. Event identity still includes story_id.
inline bool ReplayLess(const Event& a, const Event& b)
{
    if(a.hlc != b.hlc)
        return a.hlc < b.hlc;
    if(a.id.writer_id != b.id.writer_id)
        return a.id.writer_id < b.id.writer_id;
    if(a.id.incarnation != b.id.incarnation)
        return a.id.incarnation < b.id.incarnation;
    return a.id.sequence < b.id.sequence;
}
} // namespace chronolog
