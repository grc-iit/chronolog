#include "internal.h"
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace chronolog::context
{
namespace
{
using Json = nlohmann::json;
bool utf8(const std::string& value)
{
    try
    {
        static_cast<void>(Json(value).dump());
        return true;
    }
    catch(const Json::exception&)
    {
        return false;
    }
}
absl::StatusOr<client::AppendSpec>
normalize(const Memory& memory, const AgentIdentity& identity, const std::string& conversation)
{
    if(!detail::validOperationId(memory.operation_id))
        return absl::InvalidArgumentError("operation_id must be nonempty UTF-8 of at most 128 bytes, not reserved");
    client::AppendSpec spec{memory.envelope, memory.durability, memory.physical};
    if(spec.durability == Durability::Unspecified)
        spec.durability = Durability::Durable;
    if(spec.durability != Durability::Durable && spec.durability != Durability::Accepted)
        return absl::InvalidArgumentError("invalid durability");
    if(spec.envelope.content_type.empty())
        spec.envelope.content_type = "application/octet-stream";
    if(!utf8(spec.envelope.content_type) || (!spec.envelope.trace_id.empty() && spec.envelope.trace_id.size() != 16) ||
       (!spec.envelope.span_id.empty() && spec.envelope.span_id.size() != 8))
        return absl::InvalidArgumentError("invalid envelope metadata");
    const auto type = spec.envelope.content_type.substr(0, spec.envelope.content_type.find(';'));
    if(type == "application/json" || type.ends_with("+json"))
        if(Json::parse(spec.envelope.payload, nullptr, false).is_discarded())
            return absl::InvalidArgumentError("malformed declared JSON");
    for(const auto& [key, value]: spec.envelope.attributes)
        if(!utf8(key) || !utf8(value))
            return absl::InvalidArgumentError("attributes must be UTF-8");
    for(const auto& [key, value]: std::map<std::string, std::string>{{"gen_ai.agent.id", identity.agent_id},
                                                                     {"gen_ai.conversation.id", conversation},
                                                                     {"chronolog.operation.id", memory.operation_id}})
    {
        const auto found = spec.envelope.attributes.find(key);
        if(found != spec.envelope.attributes.end() && found->second != value)
            return absl::InvalidArgumentError("conflicting reserved attribute: " + key);
        spec.envelope.attributes[key] = value;
    }
    return spec;
}
std::string digest(const client::AppendSpec& spec)
{
    const auto binary = [](const std::string& value)
    { return Json::binary(std::vector<uint8_t>(value.begin(), value.end())); };
    Json physical = nullptr;
    if(spec.physical)
        physical = Json::array({spec.physical->physical_ns,
                                spec.physical->uncertainty_ns ? Json(*spec.physical->uncertainty_ns) : Json(),
                                static_cast<int>(spec.physical->status)});
    auto bytes = Json::to_cbor(Json::array({spec.envelope.content_type,
                                            binary(spec.envelope.payload),
                                            binary(spec.envelope.trace_id),
                                            binary(spec.envelope.span_id),
                                            spec.envelope.attributes,
                                            static_cast<int>(spec.durability),
                                            physical}));
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if(EVP_Digest(bytes.data(), bytes.size(), result, &size, EVP_sha256(), nullptr) != 1)
        return {};
    return std::string(reinterpret_cast<char*>(result), size);
}
MemoryResult refused(const std::string& id, absl::Status status, const detail::Record& record)
{
    MemoryResult result;
    result.current = {id,
                      std::move(status),
                      (record.state == SessionState::Fenced || record.state == SessionState::NeedsReconcile)
                              ? MemoryOutcome::Fenced
                              : MemoryOutcome::Rejected,
                      {},
                      {},
                      Durability::Unspecified};
    result.state = record.state;
    result.blocking_operation_id = record.blocking;
    return result;
}
} // namespace
namespace detail
{
bool validOperationId(const std::string& id)
{
    return !id.empty() && id.size() <= 128 && utf8(id) && !id.starts_with(marker_prefix);
}
std::string randomHex(size_t size)
{
    std::vector<unsigned char> bytes(size);
    if(RAND_bytes(bytes.data(), static_cast<int>(size)) != 1)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for(const auto byte: bytes)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
absl::StatusOr<std::string> encodeIdentity(const AgentIdentity& identity)
{
    if(identity.agent_id.empty() || identity.slot.empty() || !utf8(identity.agent_id) || !utf8(identity.slot))
        return absl::InvalidArgumentError("identity components must be nonempty UTF-8");
    return "agent-context/v2:" + Json::array({identity.agent_id, identity.slot}).dump();
}
size_t rawBytes(const Envelope& e)
{
    size_t bytes = e.payload.size() + e.content_type.size() + e.trace_id.size() + e.span_id.size();
    for(const auto& [key, value]: e.attributes) bytes += key.size() + value.size();
    return bytes;
}
bool validPosition(const Position& p, StoryId story, bool sentinel)
{
    return p.id.story_id == story && p.hlc.physical_ns >= 0 &&
           ((p.id.writer_id && p.id.incarnation && p.id.sequence) ||
            (sentinel && !p.id.writer_id && !p.id.incarnation && !p.id.sequence));
}
Deadline deadline(const ContextOptions& options, Deadline value)
{
    return value.value_or(std::chrono::system_clock::now() + options.sdk.rpc_timeout);
}
absl::StatusOr<Hlc> successor(Hlc value)
{
    if(value.logical < std::numeric_limits<uint32_t>::max())
        ++value.logical;
    else
    {
        if(value.physical_ns == std::numeric_limits<int64_t>::max())
            return absl::OutOfRangeError("HLC successor overflow");
        ++value.physical_ns;
        value.logical = 0;
    }
    return value;
}
Hlc realtime()
{
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count(),
            0};
}
client::HlcRange probeRange(Hlc end, std::chrono::nanoseconds width)
{
    return {{std::max<int64_t>(0, end.physical_ns - std::min(end.physical_ns, width.count())), 0}, end};
}
} // namespace detail
ContextClient::ContextClient(std::shared_ptr<Impl> value)
    : impl_(std::move(value))
{}
ContextClient::~ContextClient() = default;
ContextClient::ContextClient(ContextClient&&) noexcept = default;
ContextClient& ContextClient::operator=(ContextClient&&) noexcept = default;
ContextSession::ContextSession(std::shared_ptr<Impl> value)
    : impl_(std::move(value))
{}
ContextSession::~ContextSession() = default;
absl::StatusOr<ContextClient> ContextClient::Connect(ContextOptions options, Deadline deadline)
{
    if(!options.max_writable_sessions || !options.max_pending_operation_bytes || !options.max_unresolved_operations ||
       options.cut_probe_width.count() <= 0)
        return absl::InvalidArgumentError("invalid context options");
    auto sdk = client::Client::Connect(options.sdk, detail::deadline(options, deadline));
    if(!sdk.ok())
        return sdk.status();
    auto core = std::make_shared<Impl>(std::move(*sdk), std::move(options));
    core->session_id = detail::randomHex(16);
    if(core->session_id.empty())
        return absl::InternalError("session identity generation failed");
    return ContextClient(std::move(core));
}
absl::StatusOr<ContextRef>
ContextClient::ensureContext(const std::string& chronicle, const std::string& name, Deadline deadline)
{
    deadline = detail::deadline(impl_->options, deadline);
    auto c = impl_->sdk.createChronicle(chronicle, deadline);
    if(!c.ok())
    {
        if(c.status().code() != absl::StatusCode::kAlreadyExists)
            return c.status();
        c = impl_->sdk.getChronicle(chronicle, deadline);
        if(!c.ok())
            return c.status();
    }
    if(c->tombstoned)
        return absl::FailedPreconditionError("chronicle is tombstoned");
    auto stories = listContexts(chronicle, deadline);
    if(!stories.ok())
        return stories.status();
    for(const auto& story: *stories)
        if(story.name == name)
            return story;
    auto story = impl_->sdk.createStory(chronicle, name, deadline);
    if(!story.ok())
    {
        if(story.status().code() != absl::StatusCode::kAlreadyExists)
            return story.status();
        stories = listContexts(chronicle, deadline);
        if(!stories.ok())
            return stories.status();
        for(const auto& existing: *stories)
            if(existing.name == name)
                return existing;
        return absl::NotFoundError("live context not found after create race");
    }
    return ContextRef{story->id, story->chronicle, story->name};
}
absl::StatusOr<std::vector<ContextRef>> ContextClient::listContexts(const std::string& chronicle, Deadline deadline)
{
    auto stories = impl_->sdk.listStories(chronicle, detail::deadline(impl_->options, deadline));
    if(!stories.ok())
        return stories.status();
    std::vector<ContextRef> result;
    for(const auto& story: *stories)
        if(!story.tombstoned)
            result.push_back({story.id, story.chronicle, story.name});
    return result;
}
absl::StatusOr<std::shared_ptr<ContextSession>>
ContextClient::open(ContextRef context, AgentIdentity identity, OpenOptions options, Deadline deadline)
{
    deadline = detail::deadline(impl_->options, deadline);
    auto encoded = detail::encodeIdentity(identity);
    if(!encoded.ok())
        return encoded.status();
    if(options.session_id.empty())
        options.session_id = impl_->session_id;
    if(!context.story_id || !utf8(options.session_id) ||
       (options.access != Access::ReadOnly && options.access != Access::ReadWrite))
        return absl::InvalidArgumentError("invalid context generation or session_id");
    std::unique_lock open_lock(impl_->opens, std::defer_lock);
    if(!open_lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("context open busy");
    auto story = impl_->sdk.getStory(context.story_id, deadline);
    if(!story.ok())
        return story.status();
    if(story->id != context.story_id || story->tombstoned || story->chronicle != context.chronicle ||
       story->name != context.name)
        return absl::FailedPreconditionError("saved context generation does not match a live story");
    const detail::Key key{*encoded, context.story_id};
    std::shared_ptr<detail::Record> record;
    {
        std::lock_guard lock(impl_->mutex);
        auto& entry = impl_->records[{key, options.access}];
        if(!entry)
        {
            entry = std::make_shared<detail::Record>();
            entry->key = key;
            entry->context = context;
            entry->identity = identity;
            entry->access = options.access;
        }
        record = entry;
    }
    std::unique_lock record_lock(record->mutex, std::defer_lock);
    if(!record_lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("context session busy");
    if(options.resume)
    {
        const auto& checkpoint = *options.resume;
        if(checkpoint.context.story_id != context.story_id || checkpoint.identity.agent_id != identity.agent_id ||
           checkpoint.identity.slot != identity.slot)
            return absl::InvalidArgumentError("checkpoint identity or generation mismatch");
        if(checkpoint.causal_floor.physical_ns < 0 ||
           (checkpoint.processed_after && !detail::validPosition(*checkpoint.processed_after, context.story_id)))
            return absl::InvalidArgumentError("invalid checkpoint floor or processed Position");
        impl_->sdk.observeFloor(checkpoint.causal_floor);
        if(checkpoint.processed_after)
            record->processed = checkpoint.processed_after;
        if(options.access == Access::ReadWrite)
        {
            // Restored ids keep this run's attribution, which an explicit resume reuses.
            if(!record->writer)
                record->session_id = options.session_id;
            if(auto restored = detail::restore(*impl_, *record, checkpoint); !restored.ok())
                return restored;
        }
    }
    if(record->state != SessionState::Closed)
        if(auto alias = record->handle.lock())
            return alias;
    if(options.access == Access::ReadWrite && !record->counted)
    {
        std::lock_guard guard(impl_->mutex);
        if(impl_->writable >= impl_->options.max_writable_sessions)
            return absl::ResourceExhaustedError("writable session capacity");
    }
    if(options.access == Access::ReadWrite && record->writer && record->state == SessionState::Closed)
    {
        // A lost Release response is retried on the same acquisition before the next conditional Acquire (W10.12).
        auto released = record->writer->release(deadline);
        if(!released.ok())
            return released.status();
        record->release_committed = true;
        record->release_fenced = *released;
        record->writer.reset();
    }
    const auto acquire = [&](AcquireOptions acquire_options) -> absl::Status
    {
        auto writer = impl_->sdk.acquire(context.story_id, *encoded, std::move(acquire_options), deadline);
        if(!writer.ok())
            return writer.status();
        record->writer = std::move(*writer);
        auto grant = record->writer->acquisition();
        record->stamp = WriterStamp{grant.writer_id, grant.incarnation};
        // Every append of this incarnation carries a causal floor at or above this one (I7.3).
        record->acquisition_receipt = impl_->sdk.causalFloor();
        record->last_own_receipt.reset();
        record->state = SessionState::Ready;
        record->release_committed = false;
        record->release_fenced = false;
        return absl::OkStatus();
    };
    if(options.access == Access::ReadWrite && !record->writer)
    {
        record->session_id = options.session_id;
        if(options.ownership)
            record->ownership = options.ownership;
        const auto& resume = options.resume;
        if(resume && ((resume->writer && !resume->acquisition_closed) || resume->prior_state_unknown_below ||
                      resume->recovery || !resume->unresolved_operations.empty()))
        {
            record->takeover_required = resume->takeover_required || !record->stamp || !options.ownership ||
                                        !resume->acquisition || options.ownership->host_id.empty() ||
                                        options.ownership->launcher_lock_id.empty() ||
                                        options.ownership->host_id != resume->acquisition->host_id ||
                                        options.ownership->launcher_lock_id != resume->acquisition->launcher_lock_id;
            // A dead holder proven by this launcher's held stable-slot lock recovers by own-prior CAS.
            record->own_prior = !record->takeover_required;
            record->state = record->takeover_required ? SessionState::Fenced : SessionState::NeedsReconcile;
            record->recovery_required = true;
        }
        else if(record->recovery_required)
            record->state = record->takeover_required ? SessionState::Fenced : SessionState::NeedsReconcile;
        else if(record->stamp || (resume && resume->writer) ||
                (options.ownership && options.ownership->expected_prior_incarnation))
        {
            // C2: an ordinary open after a recorded close of N is conditional on N; PRIOR_MISMATCH or HELD keep
            // their typed details and change nothing.
            AcquireOptions conditional;
            conditional.expected_prior_incarnation = record->stamp ? record->stamp->incarnation
                                                     : resume && resume->writer
                                                             ? resume->writer->incarnation
                                                             : *options.ownership->expected_prior_incarnation;
            if(auto acquired = acquire(std::move(conditional)); !acquired.ok())
                return acquired;
        }
        else if(auto acquired = acquire({}); !acquired.ok())
            return acquired;
    }
    else if(options.access == Access::ReadOnly)
        record->state = SessionState::Ready;
    if(options.access == Access::ReadWrite && !record->counted)
    {
        std::lock_guard guard(impl_->mutex);
        ++impl_->writable;
        record->counted = true;
    }
    auto handle = std::shared_ptr<ContextSession>(
            new ContextSession(std::make_shared<ContextSession::Impl>(ContextSession::Impl{impl_, record})));
    record->handle = handle;
    {
        std::lock_guard io_lock(record->io_mutex);
        record->closing = false;
    }
    return handle;
}
const ContextRef& ContextSession::context() const { return impl_->record->context; }
const AgentIdentity& ContextSession::identity() const { return impl_->record->identity; }
namespace detail
{
void settle(Core& core, const std::shared_ptr<Operation>& op)
{
    std::lock_guard guard(core.mutex);
    core.pending_bytes -= op->bytes;
    op->pending.reset();
    op->bytes = 0;
    if(op->unresolved)
    {
        op->unresolved = false;
        --core.unresolved;
    }
}
void drive(Core& core,
           Record& record,
           const OperationKey& op_key,
           const std::shared_ptr<Operation>& op,
           Deadline deadline)
{
    const bool previously_uncertain = !op->result.status.ok();
    if(!op->dispatched_by)
        op->dispatched_by = record.stamp;
    auto batch = record.writer->appendBatch(std::span(&*op->pending, 1), deadline);
    auto answer = batch.ok() ? std::move(batch->front()) : absl::StatusOr<client::AppendResult>(batch.status());
    op->result.status = answer.status();
    bool completed = false;
    if(answer.ok())
    {
        op->result.receipt = *answer;
        op->result.observed_durability = answer->achieved;
        op->result.outcome =
                answer->achieved == Durability::Durable ? MemoryOutcome::Durable : MemoryOutcome::RamOnlyMayVanish;
        // One pending operation per session puts every later append of this incarnation above it (I5.5, I7.2).
        record.last_own_receipt = std::max(record.last_own_receipt.value_or(Hlc{}), answer->hlc);
        record.state = SessionState::Ready;
        completed = true;
    }
    else
    {
        const auto reason = client::rejectionOf(answer.status());
        op->result.outcome = MemoryOutcome::Unknown;
        record.state = SessionState::TransportPending;
        if(reason == AppendRejection::FencedExpired || reason == AppendRejection::FencedOwnerRemoved ||
           (batch.ok() && answer.status().code() == absl::StatusCode::kUnknown))
        {
            record.state = SessionState::NeedsReconcile;
            record.recovery_required = true;
            // A removed Keeper leaves this live Writer's own grant to recover by CAS; a confirmed terminal cause
            // recovers by plain conditional Acquire.
            record.own_prior =
                    reason != AppendRejection::FencedExpired && reason != AppendRejection::FencedOwnerRemoved;
        }
        else if(reason == AppendRejection::FencedReleased || reason == AppendRejection::FencedSuperseded ||
                (batch.ok() && answer.status().code() == absl::StatusCode::kFailedPrecondition &&
                 reason == AppendRejection::Unspecified))
        {
            record.state = SessionState::Fenced;
            record.takeover_required = true;
            record.recovery_required = true;
        }
        else if(reason == AppendRejection::StoryTombstoned)
        {
            record.state = SessionState::Closed;
            record.recovery_required = true;
        }
        else if(batch.ok() && (answer.status().code() == absl::StatusCode::kInvalidArgument ||
                               answer.status().code() == absl::StatusCode::kOutOfRange ||
                               reason == AppendRejection::SequenceGap || reason == AppendRejection::EarlierItemFailed))
        {
            if(previously_uncertain)
            {
                // A refusal cannot erase earlier uncertainty; this live Writer recovers by its own CAS.
                record.state = SessionState::NeedsReconcile;
                record.recovery_required = true;
                record.own_prior = true;
            }
            else
            {
                op->result.outcome = MemoryOutcome::Rejected;
                record.state = SessionState::Ready;
                completed = true;
            }
        }
    }
    if(completed || (batch.ok() && record.state != SessionState::TransportPending))
    {
        std::lock_guard guard(core.mutex);
        core.pending_bytes -= op->bytes;
        op->pending.reset();
        op->bytes = 0;
        if(completed)
        {
            op->unresolved = false;
            --core.unresolved;
            core.completed.push_back(op_key);
            while(core.completed.size() > core.options.max_completed_operations)
            {
                auto evicted = core.operations.find(core.completed.front());
                if(evicted != core.operations.end() && evicted->second->disposition == Disposition::None &&
                   !evicted->second->unresolved)
                    core.operations.erase(evicted);
                core.completed.pop_front();
            }
            record.blocking.reset();
        }
    }
    if(!completed)
        record.blocking = op->result.operation_id;
}
} // namespace detail
absl::StatusOr<MemoryResult> ContextSession::remember(const Memory& memory, RememberOptions options, Deadline deadline)
{
    auto& core = *impl_->core;
    auto& record = *impl_->record;
    deadline = detail::deadline(core.options, deadline);
    std::unique_lock lock(record.mutex, std::defer_lock);
    if(!lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("remember busy");
    if(record.access == Access::ReadOnly)
        return refused(memory.operation_id, absl::FailedPreconditionError("session is read-only"), record);
    const detail::OperationKey key{record.key, memory.operation_id};
    std::shared_ptr<detail::Operation> operation;
    {
        std::lock_guard guard(core.mutex);
        auto found = core.operations.find(key);
        if(found != core.operations.end())
            operation = found->second;
    }
    auto normalized = normalize(memory, record.identity, operation ? operation->conversation : record.session_id);
    if(!normalized.ok())
        return normalized.status();
    auto hash = digest(*normalized);
    if(hash.empty())
        return absl::InternalError("memory digest failed");
    // A disposition restored without a digest returns its presence or absence without revalidating content.
    if(operation && !operation->digest.empty() && operation->digest != hash)
        return absl::FailedPreconditionError("operation_id content changed");
    const bool writable = record.access == Access::ReadWrite && record.state != SessionState::Closed &&
                          record.state != SessionState::Fenced && record.state != SessionState::NeedsReconcile &&
                          !record.transition;
    if(operation && operation->disposition == detail::Disposition::Absent && options.resend_after_absent && writable &&
       (!record.blocking || *record.blocking == memory.operation_id))
    {
        // A proven ABSENT id is resent deliberately with its original specification and a new EventId.
        const size_t bytes = detail::rawBytes(normalized->envelope);
        std::lock_guard guard(core.mutex);
        if(core.unresolved >= core.options.max_unresolved_operations ||
           bytes > core.options.max_pending_operation_bytes - core.pending_bytes)
            return absl::ResourceExhaustedError("pending operation capacity");
        std::erase(record.dispositions, memory.operation_id);
        operation->disposition = detail::Disposition::None;
        operation->digest = hash;
        operation->result =
                {memory.operation_id, absl::OkStatus(), MemoryOutcome::Unknown, {}, {}, Durability::Unspecified};
        operation->dispatched_by.reset();
        operation->pending = std::move(*normalized);
        operation->bytes = bytes;
        operation->unresolved = true;
        core.pending_bytes += bytes;
        ++core.unresolved;
    }
    if(operation && (!operation->pending || record.state == SessionState::Closed))
        return MemoryResult{operation->result, {}, record.blocking, record.state};
    if(record.access != Access::ReadWrite || record.state == SessionState::Closed)
        return refused(memory.operation_id, absl::FailedPreconditionError("session is not writable"), record);
    if(!writable)
    {
        if(operation)
            return MemoryResult{operation->result, {}, record.blocking, record.state};
        return refused(memory.operation_id,
                       absl::FailedPreconditionError("session requires explicit reconciliation"),
                       record);
    }
    MemoryResult result;
    if(record.blocking && *record.blocking != memory.operation_id)
    {
        const detail::OperationKey prior_key{record.key, *record.blocking};
        std::shared_ptr<detail::Operation> prior;
        {
            std::lock_guard guard(core.mutex);
            prior = core.operations.at(prior_key);
        }
        detail::drive(core, record, prior_key, prior, deadline);
        if(record.blocking)
            return refused(memory.operation_id,
                           absl::FailedPreconditionError("pending operation blocks dispatch"),
                           record);
        result.resolved_prior.push_back(prior->result);
    }
    if(!operation)
    {
        const size_t bytes = detail::rawBytes(normalized->envelope);
        std::lock_guard guard(core.mutex);
        if(core.unresolved >= core.options.max_unresolved_operations ||
           bytes > core.options.max_pending_operation_bytes - core.pending_bytes)
            return absl::ResourceExhaustedError("pending operation capacity");
        operation = std::make_shared<detail::Operation>();
        operation->digest = hash;
        operation->conversation = record.session_id;
        operation->result =
                {memory.operation_id, absl::OkStatus(), MemoryOutcome::Unknown, {}, {}, Durability::Unspecified};
        operation->pending = std::move(*normalized);
        operation->bytes = bytes;
        core.pending_bytes += bytes;
        ++core.unresolved;
        core.operations[key] = operation;
    }
    detail::drive(core, record, key, operation, deadline);
    result.current = operation->result;
    result.state = record.state;
    result.blocking_operation_id = record.blocking;
    return result;
}
SessionStatus ContextSession::status() const
{
    auto& record = *impl_->record;
    std::lock_guard lock(record.mutex);
    SessionStatus result{record.context,
                         record.identity,
                         record.stamp,
                         record.state,
                         record.blocking,
                         {},
                         {},
                         record.reconcile_attempted,
                         impl_->core->sdk.causalFloor()};
    std::lock_guard guard(impl_->core->mutex);
    for(const auto& [key, op]: impl_->core->operations)
        if(key.first == record.key && op->unresolved && !op->marker)
            (op->disposition == detail::Disposition::PermanentUnknown ? result.permanently_unknown_operations
                                                                      : result.unresolved_operations)
                    .push_back(key.second);
    return result;
}
absl::Status ContextSession::acknowledgeProcessed(const Position& position)
{
    auto& record = *impl_->record;
    std::lock_guard lock(record.mutex);
    if(!detail::validPosition(position, record.context.story_id))
        return absl::InvalidArgumentError("invalid processed Position");
    if(record.processed &&
       std::tie(position.hlc, position.id.writer_id, position.id.incarnation, position.id.sequence) <
               std::tie(record.processed->hlc,
                        record.processed->id.writer_id,
                        record.processed->id.incarnation,
                        record.processed->id.sequence))
        return absl::FailedPreconditionError("processed Position regressed");
    record.processed = position;
    return absl::OkStatus();
}
absl::StatusOr<CloseResult> ContextSession::close(Deadline deadline)
{
    auto& record = *impl_->record;
    deadline = detail::deadline(impl_->core->options, deadline);
    {
        std::lock_guard io_lock(record.io_mutex);
        record.closing = true;
        for(const auto& [id, cancel]: record.cancellations)
        {
            static_cast<void>(id);
            cancel();
        }
    }
    std::unique_lock lock(record.mutex, std::defer_lock);
    if(!lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("close busy");
    record.recovery_required |= record.blocking.has_value();
    record.state = SessionState::Closed;
    if(record.writer)
    {
        auto released = record.writer->release(deadline);
        if(!released.ok())
            return released.status();
        record.release_committed = true;
        record.release_fenced = *released;
        record.writer.reset();
        if(record.blocking)
        {
            std::lock_guard guard(impl_->core->mutex);
            auto found = impl_->core->operations.find({record.key, *record.blocking});
            if(found != impl_->core->operations.end() && found->second->pending)
            {
                impl_->core->pending_bytes -= found->second->bytes;
                found->second->bytes = 0;
                found->second->pending.reset();
            }
        }
    }
    if(record.counted)
    {
        std::lock_guard guard(impl_->core->mutex);
        --impl_->core->writable;
        record.counted = false;
    }
    return CloseResult{record.release_committed, record.release_fenced};
}
} // namespace chronolog::context
