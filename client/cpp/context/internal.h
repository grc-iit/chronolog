#pragma once
#include "chronolog/context/context.h"
#include <deque>
#include <mutex>
#include <tuple>

namespace chronolog::context
{
namespace detail
{
using Key = std::pair<std::string, StoryId>;
using OperationKey = std::pair<Key, std::string>;
constexpr std::string_view marker_prefix = "chronolog.reconcile/";
enum class Disposition
{
    None,
    Landed,
    Absent,
    PermanentUnknown
};
struct Operation
{
    std::string digest; // Empty when restored from a checkpoint without one.
    std::string conversation;
    PriorOutcome result;
    std::optional<client::AppendSpec> pending;
    size_t bytes{};
    bool unresolved{true};
    bool marker{false};
    Disposition disposition{Disposition::None};
    // The incarnation that dispatched it after the session's lower bound; absent when unknown.
    std::optional<WriterStamp> dispatched_by;
    std::vector<WriterStamp> incarnations;
    std::optional<client::HlcRange> window;
    bool absence_provable{false};
};
struct Record
{
    ContextRef context;
    AgentIdentity identity;
    Key key;
    std::string session_id;
    Access access{Access::ReadWrite};
    SessionState state{SessionState::Ready};
    std::optional<client::Writer> writer;
    std::optional<WriterStamp> stamp;
    std::optional<std::string> blocking;
    std::optional<Position> processed;
    bool takeover_required{false};
    bool recovery_required{false};
    // NeedsReconcile of an owner proven dead or our own live Writer: own-prior CAS, takeover=true expected=N.
    bool own_prior{false};
    bool reconcile_attempted{false};
    std::optional<AcquisitionProvenance> ownership;
    // Lower bounds of the current incarnation's appends (I7.3, I7.2) and the transition in progress.
    std::optional<Hlc> acquisition_receipt;
    std::optional<Hlc> last_own_receipt;
    std::optional<ReconcileCheckpoint> transition;
    std::optional<Hlc> prior_state_unknown_below;
    // PRIOR_MISMATCH's newer incarnation, recoverable only by explicit takeover.
    std::optional<uint64_t> newer_incarnation;
    // Process-local id of an unresolved logical Acquire with the options it was dispatched with (C1, C8).
    std::optional<std::pair<std::string, AcquireOptions>> acquire;
    std::deque<std::string> dispositions;
    bool release_committed{false};
    bool release_fenced{false};
    bool counted{false};
    std::timed_mutex mutex;
    std::weak_ptr<ContextSession> handle;
    std::mutex io_mutex;
    bool closing{false};
    uint64_t next_io{};
    std::map<uint64_t, std::function<void()>> cancellations;
};
absl::StatusOr<std::string> encodeIdentity(const AgentIdentity&);
bool validOperationId(const std::string&);
std::string randomHex(size_t bytes);
size_t rawBytes(const Envelope&);
bool validPosition(const Position&, StoryId, bool sentinel = false);
Deadline deadline(const ContextOptions&, Deadline);
absl::StatusOr<Hlc> successor(Hlc);
Hlc realtime();
client::HlcRange probeRange(Hlc end, std::chrono::nanoseconds width);
class IoRegistration
{
public:
    IoRegistration(std::shared_ptr<Record> record, std::function<void()> cancel)
        : record_(std::move(record))
    {
        if(!record_)
            return;
        std::lock_guard lock(record_->io_mutex);
        id_ = ++record_->next_io;
        record_->cancellations[id_] = std::move(cancel);
        if(record_->closing)
            record_->cancellations[id_]();
    }
    ~IoRegistration()
    {
        if(!record_)
            return;
        std::lock_guard lock(record_->io_mutex);
        record_->cancellations.erase(id_);
    }

private:
    std::shared_ptr<Record> record_;
    uint64_t id_{};
};
Page readPage(client::Client&, StoryId, client::HlcRange, PageLimits, Deadline, std::shared_ptr<Record> = {});
struct VerifiedCut
{
    std::optional<Hlc> as_of;
    Page probe; // Cleared of events when no cut was verified.
    size_t calls{};
};
absl::StatusOr<VerifiedCut> verifiedCut(client::Client&,
                                        StoryId,
                                        Hlc floor,
                                        std::chrono::nanoseconds width,
                                        PageLimits,
                                        size_t max_read_calls,
                                        Deadline,
                                        std::shared_ptr<Record> = {});
// Backward suffix search for the last n events (matching `match`, when given) of [0, as_of) at a verified cut.
absl::StatusOr<LatestResult> latestSearch(client::Client&,
                                          const ContextOptions&,
                                          StoryId,
                                          size_t n,
                                          const LatestOptions&,
                                          Deadline,
                                          std::shared_ptr<Record> = {},
                                          const std::function<bool(const Event&)>& match = {});
// The n=1 lookup of the newest aggregate on a control story, for the checkpoint store; advisory before an Acquire.
absl::StatusOr<LatestResult> latestAggregate(client::Client&,
                                             const ContextOptions&,
                                             StoryId control,
                                             const std::function<bool(const Event&)>& is_aggregate,
                                             const LatestOptions& options = {},
                                             Deadline deadline = {});
struct Core
{
    Core(client::Client value, ContextOptions config)
        : sdk(std::move(value))
        , options(std::move(config))
    {}
    client::Client sdk;
    ContextOptions options;
    std::string session_id;
    std::timed_mutex opens;
    std::mutex mutex;
    size_t writable{};
    size_t pending_bytes{};
    size_t unresolved{};
    std::map<std::pair<Key, Access>, std::shared_ptr<Record>> records;
    std::map<OperationKey, std::shared_ptr<Operation>> operations;
    std::deque<OperationKey> completed;
};
} // namespace detail
struct ContextClient::Impl: detail::Core
{
    using detail::Core::Core;
};
struct ContextSession::Impl
{
    std::shared_ptr<ContextClient::Impl> core;
    std::shared_ptr<detail::Record> record;
};
namespace detail
{
// Dispatches or re-drives op on the record's Writer and applies the cause table; the caller holds record.mutex.
void drive(Core&, Record&, const OperationKey&, const std::shared_ptr<Operation>&, Deadline);
// Marks op resolved and releases its pending payload; the caller holds record.mutex, not core.mutex.
void settle(Core&, const std::shared_ptr<Operation>&);
// Restores checkpoint dispositions, unknown and unresolved ids and lower bounds into a fresh record.
absl::Status restore(Core&, Record&, const Checkpoint&);
} // namespace detail
} // namespace chronolog::context
