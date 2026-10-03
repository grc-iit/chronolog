#pragma once
#include "chronolog/client/client.h"

namespace chronolog::context
{

using Deadline = client::Deadline;
using Position = client::Position;
struct AgentIdentity
{
    std::string agent_id;
    std::string slot;
};
struct ContextRef
{
    StoryId story_id;
    std::string chronicle;
    std::string name;
};
struct WriterStamp
{
    uint64_t writer_id;
    uint64_t incarnation;
    auto operator<=>(const WriterStamp&) const = default;
};
enum class Access
{
    ReadOnly,
    ReadWrite
};
enum class SessionState
{
    Ready,
    TransportPending,
    NeedsReconcile,
    Fenced,
    Closed
};
enum class MemoryOutcome
{
    Durable,
    RamOnlyMayVanish,
    Rejected,
    Unknown,
    Fenced,
    Landed
};
enum class DeliveryLimit
{
    None,
    Events,
    Bytes,
    OversizedEvent,
    ReadCalls
};
enum class ReconcileOutcome
{
    Landed,
    Absent,
    Unknown
};
enum class FollowFrom
{
    Now,
    Beginning,
    Position
};
struct PageLimits
{
    size_t max_events{1000};
    size_t max_raw_bytes{256u << 10};
};
struct Memory
{
    std::string operation_id;
    Envelope envelope;
    Durability durability{Durability::Durable};
    std::optional<TimeReading> physical;
};
struct PriorOutcome
{
    std::string operation_id;
    absl::Status status;
    MemoryOutcome outcome;
    std::optional<client::AppendResult> receipt;
    std::optional<Position> landed;
    Durability observed_durability{Durability::Unspecified};
};
struct MemoryResult
{
    PriorOutcome current;
    std::vector<PriorOutcome> resolved_prior;
    std::optional<std::string> blocking_operation_id;
    SessionState state;
};
struct RememberOptions
{
    bool resend_after_absent{false};
};
struct Page
{
    std::vector<Event> events;
    std::optional<client::HlcRange> range;
    std::optional<Completion> completion;
    std::optional<client::HlcRange> completion_range;
    absl::Status stream_status;
    DeliveryLimit limited{DeliveryLimit::None};
    size_t raw_bytes{};
    bool answer_complete{false};
    bool has_more{false};
    bool idle{false};
    bool cut_covers_causal_floor{true};
    std::optional<Position> after;
    std::optional<std::string> next_cursor;
    std::optional<Hlc> delivered_prefix_end;
};
struct RecallOptions
{
    std::optional<Hlc> start;
    std::optional<Hlc> end;
    std::optional<std::string> cursor;
    PageLimits limits;
    size_t max_read_calls{32};
};
struct LatestOptions
{
    std::optional<Hlc> before;
    PageLimits limits;
    size_t max_read_calls{32};
};
struct LatestResult
{
    Page page;
    std::optional<Hlc> as_of;
    bool selection_complete{false};
};
struct AcquisitionProvenance
{
    std::string host_id;
    std::string launcher_lock_id;
    std::optional<uint64_t> expected_prior_incarnation;
    std::optional<Hlc> acquisition_record_receipt_hlc;
};
struct RecoveryIncarnation
{
    WriterStamp writer;
    std::optional<std::string> marker_operation_id;
};
struct ReconcileCheckpoint
{
    std::string transition_id;
    Hlc lower_bound;
    std::vector<RecoveryIncarnation> recovered_incarnations;
    std::optional<std::string> current_marker_operation_id;
    std::optional<Hlc> marker_hlc;
};
struct ReconciledOperation
{
    std::string operation_id;
    ReconcileOutcome outcome;
    std::optional<Position> landed;
    Durability observed_durability{Durability::Unspecified};
};
struct OperationDisposition
{
    ReconciledOperation result; // Checkpoint persists only Landed or Absent here.
    WriterStamp prior_writer;
    std::optional<std::string> normalized_digest;
};
struct UnknownOperation
{
    std::string operation_id;
    // Incarnations that may hold it and the proof window [l, m) whose later complete Read releases it (C5).
    std::vector<WriterStamp> incarnations;
    std::optional<client::HlcRange> window;
    // False for an id supplied without a known dispatch bound: such an id can only be found LANDED.
    bool absence_provable{false};
    std::optional<std::string> normalized_digest;
};
struct Checkpoint
{
    AgentIdentity identity;
    ContextRef context;
    Hlc causal_floor;
    std::optional<Position> processed_after;
    std::optional<WriterStamp> writer;
    std::optional<AcquisitionProvenance> acquisition;
    std::optional<Hlc> last_own_receipt_hlc;
    std::optional<ReconcileCheckpoint> recovery;
    std::vector<std::string> unresolved_operations;
    std::vector<UnknownOperation> permanently_unknown_operations;
    std::vector<OperationDisposition> dispositions;
    std::optional<Hlc> prior_state_unknown_below;
    bool acquisition_closed{false};
    bool reconcile_attempted{false};
    bool takeover_required{false};
};
struct OpenOptions
{
    Access access{Access::ReadWrite};
    std::string session_id;
    std::optional<Checkpoint> resume;
    std::optional<AcquisitionProvenance> ownership;
};
struct ReconcileOptions
{
    std::vector<std::string> operation_ids;
    bool takeover{false};
    size_t max_read_calls{32};
};
struct ReconcileResult
{
    std::optional<WriterStamp> writer;
    std::optional<Hlc> marker_hlc;
    std::optional<client::HlcRange> range;
    std::vector<ReconciledOperation> operations;
    std::optional<Completion> completion;
    absl::Status status;
    bool attempted{false};
    std::vector<std::string> permanently_unknown_operations;
    bool proof_complete{false};
    bool supply_all_unseen_operation_ids{true};
    bool omitted_operation_ids_may_duplicate{true};
};
struct CloseResult
{
    bool release_committed{false};
    bool fenced{false};
};
struct SessionStatus
{
    ContextRef context;
    AgentIdentity identity;
    std::optional<WriterStamp> writer;
    SessionState state;
    std::optional<std::string> blocking_operation_id;
    std::vector<std::string> unresolved_operations;
    std::vector<std::string> permanently_unknown_operations;
    bool reconcile_attempted{false};
    Hlc causal_floor;
};
struct ContextOptions
{
    client::ClientOptions sdk;
    size_t max_writable_sessions{32};
    size_t max_pending_operation_bytes{64u << 20};
    size_t max_completed_operations{10000};
    size_t max_unresolved_operations{10000};
    size_t max_persisted_dispositions{10000};
    size_t max_checkpoint_payload_bytes{1u << 20};
    std::chrono::nanoseconds cut_probe_width{std::chrono::seconds(1)};
};
// Versioned self-contained encoding of a Checkpoint; RESOURCE_EXHAUSTED above max_bytes.
absl::StatusOr<std::string> encodeCheckpoint(const Checkpoint&, size_t max_bytes = 1u << 20);
absl::StatusOr<Checkpoint> decodeCheckpoint(std::string_view);
class ContextSession;
struct FollowInput
{
    std::shared_ptr<ContextSession> session;
    FollowFrom from{FollowFrom::Now};
    std::optional<Position> after{};
};
struct FollowOptions
{
    PageLimits limits;
    std::chrono::seconds wait{5};
};
struct FollowPage
{
    ContextRef context;
    Page page;
    std::optional<Position> resume;
    std::optional<Hlc> starting_cut;
    std::vector<std::string> uncertified_route_keepers;
};
struct FollowResult
{
    std::vector<FollowPage> pages;
    absl::Status status;
    bool idle{false};
};
class ContextClient
{
public:
    ~ContextClient();
    ContextClient(ContextClient&&) noexcept;
    ContextClient& operator=(ContextClient&&) noexcept;
    ContextClient(const ContextClient&) = delete;
    ContextClient& operator=(const ContextClient&) = delete;
    static absl::StatusOr<ContextClient> Connect(ContextOptions, Deadline deadline = {});
    absl::StatusOr<ContextRef>
    ensureContext(const std::string& chronicle, const std::string& name, Deadline deadline = {});
    absl::StatusOr<std::vector<ContextRef>> listContexts(const std::string& chronicle, Deadline deadline = {});
    absl::StatusOr<std::shared_ptr<ContextSession>>
    open(ContextRef, AgentIdentity, OpenOptions options = {}, Deadline deadline = {});
    absl::StatusOr<FollowResult>
    follow(std::span<const FollowInput>, FollowOptions options = {}, Deadline deadline = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit ContextClient(std::shared_ptr<Impl>);
    friend class ContextSession;
};
class ContextSession
{
public:
    ~ContextSession();
    ContextSession(const ContextSession&) = delete;
    ContextSession& operator=(const ContextSession&) = delete;
    const ContextRef& context() const;
    const AgentIdentity& identity() const;
    absl::StatusOr<MemoryResult> remember(const Memory&, RememberOptions options = {}, Deadline deadline = {});
    absl::StatusOr<Page> recall(RecallOptions options = {}, Deadline deadline = {});
    absl::StatusOr<LatestResult> latest(size_t n, LatestOptions options = {}, Deadline deadline = {});
    absl::StatusOr<ReconcileResult> reconcile(ReconcileOptions options = {}, Deadline deadline = {});
    absl::Status acknowledgeProcessed(const Position&);
    Checkpoint checkpoint() const;
    SessionStatus status() const;
    absl::StatusOr<CloseResult> close(Deadline deadline = {});

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit ContextSession(std::shared_ptr<Impl>);
    friend class ContextClient;
};
} // namespace chronolog::context
