#include "internal.h"
#include <limits>
#include <nlohmann/json.hpp>
#include <set>

namespace chronolog::context
{
namespace
{
using Json = nlohmann::json;
using detail::Disposition;
using detail::Operation;
using OperationPtr = std::shared_ptr<Operation>;
constexpr std::string_view checkpoint_version = "chronolog-context-checkpoint/v1";
constexpr std::string_view marker_version = "chronolog-reconcile-marker/v1";

bool contains(const std::vector<WriterStamp>& set, const WriterStamp& stamp)
{
    return std::find(set.begin(), set.end(), stamp) != set.end();
}
std::vector<WriterStamp> stamps(const ReconcileCheckpoint& transition)
{
    std::vector<WriterStamp> result;
    for(const auto& recovered: transition.recovered_incarnations) result.push_back(recovered.writer);
    return result;
}
OperationPtr find(detail::Core& core, const detail::Record& record, const std::string& id)
{
    std::lock_guard guard(core.mutex);
    auto found = core.operations.find({record.key, id});
    return found == core.operations.end() ? nullptr : found->second;
}
std::vector<std::pair<std::string, OperationPtr>> operations(detail::Core& core, const detail::Record& record)
{
    std::lock_guard guard(core.mutex);
    std::vector<std::pair<std::string, OperationPtr>> result;
    for(const auto& [key, op]: core.operations)
        if(key.first == record.key)
            result.emplace_back(key.second, op);
    return result;
}
ReconciledOperation describe(const std::string& id, const Operation& op)
{
    if(op.disposition == Disposition::Landed)
        return {id, ReconcileOutcome::Landed, op.result.landed, op.result.observed_durability};
    if(op.disposition == Disposition::Absent || (!op.unresolved && op.result.outcome == MemoryOutcome::Rejected))
        return {id, ReconcileOutcome::Absent, {}, Durability::Unspecified};
    if(!op.unresolved && op.result.receipt)
        return {id,
                ReconcileOutcome::Landed,
                Position{op.result.receipt->hlc, op.result.receipt->event_id},
                op.result.receipt->achieved};
    return {id, ReconcileOutcome::Unknown, {}, Durability::Unspecified};
}
// Retains a LANDED or ABSENT disposition in the record's bounded FIFO; the caller holds core.mutex.
void keep(detail::Core& core, detail::Record& record, const std::string& id)
{
    std::erase(record.dispositions, id);
    record.dispositions.push_back(id);
    while(record.dispositions.size() > core.options.max_persisted_dispositions)
    {
        auto found = core.operations.find({record.key, record.dispositions.front()});
        if(found != core.operations.end() &&
           (found->second->disposition == Disposition::Landed || found->second->disposition == Disposition::Absent))
            core.operations.erase(found);
        record.dispositions.pop_front();
    }
}
void land(detail::Core& core, detail::Record& record, const std::string& id, Operation& op, const Event& event)
{
    std::lock_guard guard(core.mutex);
    core.pending_bytes -= op.bytes;
    op.pending.reset();
    op.bytes = 0;
    if(op.unresolved)
        --core.unresolved;
    op.unresolved = false;
    op.disposition = Disposition::Landed;
    // Observed presence, not a recovered original acknowledgement.
    op.result = {id, absl::OkStatus(), MemoryOutcome::Landed, {}, Position{event.hlc, event.id}, event.durability};
    keep(core, record, id);
}
void absent(detail::Core& core, detail::Record& record, const std::string& id, Operation& op)
{
    std::lock_guard guard(core.mutex);
    core.pending_bytes -= op.bytes;
    op.pending.reset();
    op.bytes = 0;
    if(op.unresolved)
        --core.unresolved;
    op.unresolved = false;
    op.disposition = Disposition::Absent;
    op.result = {id,
                 absl::FailedPreconditionError("reconcile proved this operation ABSENT; resend it explicitly"),
                 MemoryOutcome::Rejected,
                 {},
                 {},
                 Durability::Unspecified};
    keep(core, record, id);
}
void unknownForever(detail::Core& core,
                    const std::string& id,
                    Operation& op,
                    client::HlcRange window,
                    const std::vector<WriterStamp>& incarnations,
                    bool provable)
{
    std::lock_guard guard(core.mutex);
    core.pending_bytes -= op.bytes;
    op.pending.reset();
    op.bytes = 0;
    op.disposition = Disposition::PermanentUnknown;
    op.window = window;
    op.incarnations = incarnations;
    op.absence_provable = provable;
    op.result = {id,
                 absl::FailedPreconditionError("operation is permanently UNKNOWN after an incomplete reconcile; "
                                               "resend is refused"),
                 MemoryOutcome::Unknown,
                 {},
                 {},
                 Durability::Unspecified};
}
struct Proof
{
    std::map<std::string, Event> landed;
    bool complete{false};
    std::optional<Completion> completion;
    absl::Status status;
};
// Reads window to a complete answer through certified continuations and keeps the user events of incarnations
// (I6.12). Presence is evidence from any delivered event; absence needs the complete answer.
Proof prove(detail::Core& core,
            const std::shared_ptr<detail::Record>& record,
            client::HlcRange window,
            const std::vector<WriterStamp>& incarnations,
            size_t& calls,
            size_t max_calls,
            Deadline deadline)
{
    Proof proof;
    Hlc start = window.start;
    if(start >= window.end)
    {
        proof.complete = true;
        return proof;
    }
    while(calls < max_calls)
    {
        auto page = detail::readPage(core.sdk, record->context.story_id, {start, window.end}, {}, deadline, record);
        ++calls;
        proof.completion = page.completion;
        proof.status = page.stream_status;
        for(const auto& event: page.events)
        {
            const auto id = event.envelope.attributes.find("chronolog.operation.id");
            if(id != event.envelope.attributes.end() && !id->second.starts_with(detail::marker_prefix) &&
               contains(incarnations, {event.id.writer_id, event.id.incarnation}))
                proof.landed.try_emplace(id->second, event);
        }
        if(page.answer_complete)
        {
            proof.complete = true;
            break;
        }
        if(!page.has_more || !page.delivered_prefix_end || *page.delivered_prefix_end <= start)
            break;
        start = *page.delivered_prefix_end;
    }
    if(!proof.complete && proof.status.ok() && calls >= max_calls)
        proof.status = absl::ResourceExhaustedError("max_read_calls exhausted before the proof completed");
    return proof;
}
// C5: a later complete Read of a permanently UNKNOWN id's recorded window releases it; its old incarnations are
// fenced, so the window is stable (I6.8).
void release(detail::Core& core,
             const std::shared_ptr<detail::Record>& record,
             size_t& calls,
             size_t max_calls,
             Deadline deadline,
             const std::set<std::string>& skip,
             ReconcileResult& result)
{
    using Window = std::pair<std::pair<Hlc, Hlc>, std::vector<WriterStamp>>;
    std::map<Window, std::vector<std::pair<std::string, OperationPtr>>> groups;
    for(auto& [id, op]: operations(core, *record))
        if(op->disposition == Disposition::PermanentUnknown && op->window && !skip.contains(id))
            groups[{{op->window->start, op->window->end}, op->incarnations}].emplace_back(id, op);
    for(const auto& [window, members]: groups)
    {
        if(calls >= max_calls)
            break;
        auto proof = prove(core,
                           record,
                           {window.first.first, window.first.second},
                           window.second,
                           calls,
                           max_calls,
                           deadline);
        for(const auto& [id, op]: members)
        {
            if(auto found = proof.landed.find(id); found != proof.landed.end())
                land(core, *record, id, *op, found->second);
            else if(proof.complete && op->absence_provable)
                absent(core, *record, id, *op);
            else
                continue;
            result.operations.push_back(describe(id, *op));
        }
    }
}
Json hlc(const Hlc& value) { return Json::array({value.physical_ns, value.logical}); }
Json stamp(const WriterStamp& value) { return Json::array({value.writer_id, value.incarnation}); }
Json range(const client::HlcRange& value) { return Json::array({hlc(value.start), hlc(value.end)}); }
Json position(const Position& p)
{
    return Json::array(
            {p.hlc.physical_ns, p.hlc.logical, p.id.story_id, p.id.writer_id, p.id.incarnation, p.id.sequence});
}
template <class T, class F>
Json optional(const std::optional<T>& value, F encode)
{
    return value ? Json(encode(*value)) : Json();
}
std::string hex(const std::string& bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for(const unsigned char byte: bytes)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
Json provenance(const ReconcileCheckpoint& transition,
                const WriterStamp& next,
                const std::vector<std::string>& unresolved,
                const std::vector<std::string>& unknown)
{
    Json recovered = Json::array();
    for(const auto& value: transition.recovered_incarnations) recovered.push_back(stamp(value.writer));
    return {{"v", marker_version},
            {"transition", transition.transition_id},
            {"incarnation", stamp(next)},
            {"recovered", recovered},
            {"unresolved", unresolved},
            {"permanently_unknown", unknown}};
}
template <class T>
T number(const Json& value)
{
    if(!value.is_number_integer())
        throw std::invalid_argument("expected an integer");
    if constexpr(std::is_signed_v<T>)
    {
        if(value.is_number_unsigned() &&
           value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw std::out_of_range("integer out of range");
        const auto result = value.get<int64_t>();
        if(result < std::numeric_limits<T>::min() || result > std::numeric_limits<T>::max())
            throw std::out_of_range("integer out of range");
        return static_cast<T>(result);
    }
    else
    {
        if(!value.is_number_unsigned() || value.get<uint64_t>() > std::numeric_limits<T>::max())
            throw std::out_of_range("unsigned integer out of range");
        return static_cast<T>(value.get<uint64_t>());
    }
}
Hlc toHlc(const Json& value)
{
    if(value.size() != 2)
        throw std::invalid_argument("hlc");
    Hlc result{number<int64_t>(value.at(0)), number<uint32_t>(value.at(1))};
    if(result.physical_ns < 0)
        throw std::out_of_range("negative hlc");
    return result;
}
WriterStamp toStamp(const Json& value)
{
    if(value.size() != 2)
        throw std::invalid_argument("stamp");
    return {number<uint64_t>(value.at(0)), number<uint64_t>(value.at(1))};
}
client::HlcRange toRange(const Json& value)
{
    if(value.size() != 2)
        throw std::invalid_argument("range");
    client::HlcRange result{toHlc(value.at(0)), toHlc(value.at(1))};
    if(result.end < result.start)
        throw std::invalid_argument("reversed range");
    return result;
}
Position toPosition(const Json& value)
{
    if(value.size() != 6)
        throw std::invalid_argument("position");
    return {{number<int64_t>(value.at(0)), number<uint32_t>(value.at(1))},
            {number<uint64_t>(value.at(2)),
             number<uint64_t>(value.at(3)),
             number<uint64_t>(value.at(4)),
             number<uint64_t>(value.at(5))}};
}
template <class T, class F>
std::optional<T> toOptional(const Json& value, F decode)
{
    if(value.is_null())
        return std::nullopt;
    return decode(value);
}
std::string unhex(const std::string& value)
{
    if(value.size() % 2)
        throw std::invalid_argument("odd hex digest");
    std::string result;
    for(size_t i = 0; i < value.size(); i += 2)
    {
        const auto digit = [](char c)
        {
            if(c >= '0' && c <= '9')
                return c - '0';
            if(c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            throw std::invalid_argument("invalid hex digest");
        };
        result += static_cast<char>(digit(value[i]) * 16 + digit(value[i + 1]));
    }
    return result;
}
std::string text(const Json& value)
{
    if(!value.is_string())
        throw std::invalid_argument("expected a string");
    return value.get<std::string>();
}
} // namespace
absl::StatusOr<ReconcileResult> ContextSession::reconcile(ReconcileOptions options, Deadline deadline)
{
    auto& core = *impl_->core;
    auto& record = *impl_->record;
    deadline = detail::deadline(core.options, deadline);
    if(!options.max_read_calls)
        return absl::InvalidArgumentError("max_read_calls must be positive");
    for(const auto& id: options.operation_ids)
        if(!detail::validOperationId(id))
            return absl::InvalidArgumentError("invalid operation_id");
    std::unique_lock lock(record.mutex, std::defer_lock);
    if(!lock.try_lock_until(*deadline))
        return absl::DeadlineExceededError("reconcile busy");
    if(record.access != Access::ReadWrite)
        return absl::FailedPreconditionError("session is read-only");
    if(record.state == SessionState::Closed)
        return absl::FailedPreconditionError("session is closed; reopen it to reconcile");
    ReconcileResult result;
    size_t calls = 0;
    const auto finish = [&]
    {
        result.writer = record.stamp;
        for(const auto& [id, op]: operations(core, record))
            if(op->disposition == Disposition::PermanentUnknown)
                result.permanently_unknown_operations.push_back(id);
        return std::move(result);
    };
    // 1. Re-drive a transport or route pending call first; takeover never bypasses it.
    if(record.writer && record.blocking && record.state == SessionState::TransportPending)
    {
        auto op = find(core, record, *record.blocking);
        if(op && op->pending)
            detail::drive(core, record, {record.key, *record.blocking}, op, deadline);
        if(record.state == SessionState::TransportPending)
        {
            result.status = op ? op->result.status : absl::UnavailableError("pending operation unresolved");
            return finish();
        }
    }
    auto& transition = record.transition;
    if(transition && transition->current_marker_operation_id && !transition->marker_hlc)
        if(auto marker = find(core, record, *transition->current_marker_operation_id);
           marker && !marker->unresolved && marker->result.receipt)
            transition->marker_hlc = marker->result.receipt->hlc;
    size_t unseen = 0;
    for(const auto& id: std::set(options.operation_ids.begin(), options.operation_ids.end()))
        unseen += !find(core, record, id);
    {
        std::lock_guard guard(core.mutex);
        if(core.unresolved + unseen + 1 > core.options.max_unresolved_operations)
            return absl::ResourceExhaustedError("unresolved operation capacity");
    }
    if(!record.recovery_required && !transition)
    {
        // Nothing to recover: a later complete Read of a recorded window can still release permanent ids (C5).
        release(core, impl_->record, calls, options.max_read_calls, deadline, {}, result);
        for(const auto& id: options.operation_ids)
            if(auto op = find(core, record, id))
                if(std::none_of(result.operations.begin(),
                                result.operations.end(),
                                [&](const ReconciledOperation& known) { return known.operation_id == id; }))
                    result.operations.push_back(describe(id, *op));
        return finish();
    }
    if(!(transition && transition->marker_hlc && record.writer))
    {
        if((record.takeover_required || !record.stamp) && !options.takeover)
            return absl::FailedPreconditionError(
                    "RELEASED, SUPERSEDED or unproven ownership requires reconcile with takeover=true");
        std::vector<std::string> unresolved = options.operation_ids;
        std::vector<std::string> unknown;
        for(const auto& [id, op]: operations(core, record))
            if(op->unresolved && !op->marker)
                (op->disposition == Disposition::PermanentUnknown ? unknown : unresolved).push_back(id);
        // 2. Capture the prior stamp and the earliest lower bound l of this transition.
        if(!transition)
        {
            ReconcileCheckpoint started;
            started.transition_id = detail::randomHex(16);
            if(started.transition_id.empty())
                return absl::InternalError("transition id generation failed");
            // Unknown prior bounds fall back to 0; nothing below an unknown bound can be ABSENT.
            if(!record.prior_state_unknown_below)
                started.lower_bound =
                        std::max(record.acquisition_receipt.value_or(Hlc{}), record.last_own_receipt.value_or(Hlc{}));
            transition = std::move(started);
        }
        const auto recover = [&](WriterStamp writer, std::optional<std::string> marker)
        {
            if(!contains(stamps(*transition), writer))
                transition->recovered_incarnations.push_back({writer, std::move(marker)});
        };
        if(record.stamp)
            recover(*record.stamp, transition->current_marker_operation_id);
        AcquireOptions acquire;
        if(record.takeover_required || !record.stamp)
        {
            acquire.takeover = true;
            if(record.newer_incarnation && record.stamp)
            {
                // C1: an unknown newer incarnation named by PRIOR_MISMATCH joins the proof set.
                recover({record.stamp->writer_id, *record.newer_incarnation}, std::nullopt);
                acquire.expected_prior_incarnation = record.newer_incarnation;
            }
            else if(record.stamp)
                acquire.expected_prior_incarnation = record.stamp->incarnation;
        }
        else
        {
            acquire.takeover = record.own_prior;
            acquire.expected_prior_incarnation = record.stamp->incarnation;
        }
        const auto bound = provenance(*transition,
                                      {std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()},
                                      unresolved,
                                      unknown)
                                   .dump();
        if(bound.size() > core.options.max_checkpoint_payload_bytes)
            return absl::ResourceExhaustedError("resume marker provenance exceeds max_checkpoint_payload_bytes");
        transition->current_marker_operation_id.reset();
        // 3. Acquire only at this recovery boundary; one logical Acquire keeps one process-local id (C1, C8).
        if(record.acquire && record.acquire->second.takeover == acquire.takeover &&
           record.acquire->second.expected_prior_incarnation == acquire.expected_prior_incarnation)
            acquire.acquire_request_id = record.acquire->first;
        else
        {
            auto minted = core.sdk.newAcquireRequestId();
            if(!minted.ok())
                return minted.status();
            acquire.acquire_request_id = std::move(*minted);
        }
        record.acquire.emplace(acquire.acquire_request_id, acquire);
        auto writer = core.sdk.acquire(record.context.story_id, record.key.first, acquire, deadline);
        if(!writer.ok())
        {
            const auto refusal = client::acquireRefusalOf(writer.status());
            if(refusal)
                record.acquire.reset();
            if(refusal && refusal->refusal_reason == AcquireRefusalReason::PriorMismatch)
            {
                // A newer holder is never superseded without the caller's explicit takeover (C1).
                record.newer_incarnation = refusal->current_incarnation;
                record.takeover_required = true;
                record.state = SessionState::Fenced;
            }
            result.status = writer.status();
            return finish();
        }
        record.acquire.reset();
        record.writer = std::move(*writer);
        const auto grant = record.writer->acquisition();
        record.stamp = WriterStamp{grant.writer_id, grant.incarnation};
        record.acquisition_receipt = core.sdk.causalFloor();
        record.last_own_receipt.reset();
        record.newer_incarnation.reset();
        record.takeover_required = false;
        record.own_prior = false;
        record.release_committed = false;
        record.release_fenced = false;
        record.blocking.reset();
        record.state = SessionState::NeedsReconcile;
        // 4. One DURABLE resume marker per transition and incarnation.
        const auto marker_id = std::string(detail::marker_prefix) + transition->transition_id + "/" +
                               std::to_string(record.stamp->incarnation);
        auto marker = std::make_shared<Operation>();
        marker->marker = true;
        marker->conversation = record.session_id;
        marker->result = {marker_id, absl::OkStatus(), MemoryOutcome::Unknown, {}, {}, Durability::Unspecified};
        client::AppendSpec spec;
        spec.durability = Durability::Durable;
        spec.envelope.content_type = "application/vnd.chronolog.reconcile-marker+json";
        spec.envelope.payload = provenance(*transition, *record.stamp, unresolved, unknown).dump();
        spec.envelope.attributes = {{"gen_ai.agent.id", record.identity.agent_id},
                                    {"gen_ai.conversation.id", record.session_id},
                                    {"chronolog.operation.id", marker_id}};
        marker->bytes = detail::rawBytes(spec.envelope);
        marker->pending = std::move(spec);
        {
            std::lock_guard guard(core.mutex);
            if(marker->bytes > core.options.max_pending_operation_bytes - core.pending_bytes)
                return absl::ResourceExhaustedError("pending operation capacity");
            core.pending_bytes += marker->bytes;
            ++core.unresolved;
            core.operations[{record.key, marker_id}] = marker;
        }
        transition->current_marker_operation_id = marker_id;
        transition->marker_hlc.reset();
        detail::drive(core, record, {record.key, marker_id}, marker, deadline);
        if(!marker->result.receipt)
        {
            // A refused marker leaves this live incarnation to recover by its own CAS.
            if(!marker->unresolved || record.state == SessionState::Ready)
            {
                record.state = SessionState::NeedsReconcile;
                record.own_prior = true;
                record.recovery_required = true;
            }
            result.status = marker->result.status;
            return finish();
        }
        transition->marker_hlc = marker->result.receipt->hlc;
    }
    // 5. Read [l, m) filtering every old and intermediate incarnation; the final incarnation is excluded.
    const client::HlcRange window{transition->lower_bound, *transition->marker_hlc};
    const auto incarnations = stamps(*transition);
    auto proof = prove(core, impl_->record, window, incarnations, calls, options.max_read_calls, deadline);
    record.reconcile_attempted = true;
    result.attempted = true;
    result.range = window;
    result.marker_hlc = window.end;
    result.completion = proof.completion;
    result.status = proof.status;
    result.proof_complete = proof.complete;
    // 6. LANDED, ABSENT or permanent UNKNOWN per supplied, retained and discovered id.
    std::vector<std::string> targets = options.operation_ids;
    for(const auto& [id, op]: operations(core, record))
        if(op->unresolved && op->disposition == Disposition::None && !op->marker)
            targets.push_back(id);
    for(const auto& [id, event]: proof.landed) targets.push_back(id);
    std::set<std::string> seen;
    std::set<std::string> made_unknown;
    for(const auto& id: targets)
    {
        if(!seen.insert(id).second)
            continue;
        auto op = find(core, record, id);
        const auto landed = proof.landed.find(id);
        if(!op)
        {
            op = std::make_shared<Operation>();
            op->conversation = record.session_id;
            op->result = {id, absl::OkStatus(), MemoryOutcome::Unknown, {}, {}, Durability::Unspecified};
            std::lock_guard guard(core.mutex);
            ++core.unresolved;
            core.operations[{record.key, id}] = op;
        }
        if(op->unresolved && op->disposition == Disposition::None)
        {
            const bool bounded = op->dispatched_by && contains(incarnations, *op->dispatched_by);
            if(landed != proof.landed.end())
                land(core, record, id, *op, landed->second);
            else if(proof.complete && bounded)
                absent(core, record, id, *op);
            else
            {
                unknownForever(core, id, *op, window, incarnations, bounded);
                made_unknown.insert(id);
            }
        }
        result.operations.push_back(describe(id, *op));
    }
    release(core, impl_->record, calls, options.max_read_calls, deadline, made_unknown, result);
    // 7. The attempt is recorded even when incomplete; a usable successor with nothing pending admits new ids.
    for(const auto& recovered: transition->recovered_incarnations)
        if(recovered.marker_operation_id)
            if(auto marker = find(core, record, *recovered.marker_operation_id))
                detail::settle(core, marker);
    transition.reset();
    record.recovery_required = false;
    record.prior_state_unknown_below.reset();
    record.blocking.reset();
    record.state = SessionState::Ready;
    return finish();
}
Checkpoint ContextSession::checkpoint() const
{
    auto& core = *impl_->core;
    auto& record = *impl_->record;
    std::lock_guard lock(record.mutex);
    Checkpoint result;
    result.identity = record.identity;
    result.context = record.context;
    result.causal_floor = core.sdk.causalFloor();
    result.processed_after = record.processed;
    result.writer = record.stamp;
    if(record.ownership || record.acquisition_receipt)
    {
        result.acquisition = record.ownership.value_or(AcquisitionProvenance{});
        result.acquisition->acquisition_record_receipt_hlc = record.acquisition_receipt;
    }
    result.last_own_receipt_hlc = record.last_own_receipt;
    result.recovery = record.transition;
    const auto known = operations(core, record);
    for(const auto& [id, op]: known)
    {
        if(op->marker || !op->unresolved)
            continue;
        if(op->disposition == Disposition::PermanentUnknown)
            result.permanently_unknown_operations.push_back(
                    {id,
                     op->incarnations,
                     op->window,
                     op->absence_provable,
                     op->digest.empty() ? std::nullopt : std::optional(op->digest)});
        else
            result.unresolved_operations.push_back(id);
    }
    for(const auto& id: record.dispositions)
    {
        auto op = std::find_if(known.begin(), known.end(), [&](const auto& entry) { return entry.first == id; });
        if(op == known.end() ||
           (op->second->disposition != Disposition::Landed && op->second->disposition != Disposition::Absent))
            continue;
        const auto& value = *op->second;
        WriterStamp prior = value.dispatched_by.value_or(WriterStamp{});
        if(value.result.landed)
            prior = {value.result.landed->id.writer_id, value.result.landed->id.incarnation};
        result.dispositions.push_back(
                {describe(id, value), prior, value.digest.empty() ? std::nullopt : std::optional(value.digest)});
    }
    result.prior_state_unknown_below = record.prior_state_unknown_below;
    result.acquisition_closed = record.release_committed && !record.recovery_required;
    result.reconcile_attempted = record.reconcile_attempted;
    result.takeover_required = record.takeover_required;
    return result;
}
namespace detail
{
absl::Status restore(detail::Core& core, Record& record, const Checkpoint& checkpoint)
{
    if(checkpoint.dispositions.size() > core.options.max_persisted_dispositions)
        return absl::ResourceExhaustedError("checkpoint dispositions exceed max_persisted_dispositions");
    for(const auto& value: checkpoint.dispositions)
        if(!validOperationId(value.result.operation_id) || value.result.outcome == ReconcileOutcome::Unknown)
            return absl::InvalidArgumentError("checkpoint disposition must be a LANDED or ABSENT operation id");
    for(const auto& value: checkpoint.permanently_unknown_operations)
        if(!validOperationId(value.operation_id))
            return absl::InvalidArgumentError("invalid permanently UNKNOWN operation id");
    for(const auto& id: checkpoint.unresolved_operations)
        if(!validOperationId(id))
            return absl::InvalidArgumentError("invalid unresolved operation id");
    std::lock_guard guard(core.mutex);
    size_t added = 0;
    for(const auto& value: checkpoint.permanently_unknown_operations)
        added += !core.operations.contains({record.key, value.operation_id});
    for(const auto& id: checkpoint.unresolved_operations) added += !core.operations.contains({record.key, id});
    if(core.unresolved + added > core.options.max_unresolved_operations)
        return absl::ResourceExhaustedError("checkpoint exceeds unresolved operation capacity");
    // Later in-process state wins over an older checkpoint; only ids this process does not know are restored.
    for(const auto& value: checkpoint.dispositions)
    {
        const auto& id = value.result.operation_id;
        if(core.operations.contains({record.key, id}))
            continue;
        auto op = std::make_shared<Operation>();
        op->digest = value.normalized_digest.value_or("");
        op->conversation = record.session_id;
        op->unresolved = false;
        op->dispatched_by = value.prior_writer;
        if(value.result.outcome == ReconcileOutcome::Landed)
        {
            op->disposition = Disposition::Landed;
            op->result = {id,
                          absl::OkStatus(),
                          MemoryOutcome::Landed,
                          {},
                          value.result.landed,
                          value.result.observed_durability};
        }
        else
        {
            op->disposition = Disposition::Absent;
            op->result = {id,
                          absl::FailedPreconditionError("reconcile proved this operation ABSENT; resend it explicitly"),
                          MemoryOutcome::Rejected,
                          {},
                          {},
                          Durability::Unspecified};
        }
        core.operations[{record.key, id}] = op;
        keep(core, record, id);
    }
    for(const auto& value: checkpoint.permanently_unknown_operations)
    {
        if(core.operations.contains({record.key, value.operation_id}))
            continue;
        auto op = std::make_shared<Operation>();
        op->digest = value.normalized_digest.value_or("");
        op->conversation = record.session_id;
        op->disposition = Disposition::PermanentUnknown;
        op->incarnations = value.incarnations;
        op->window = value.window;
        op->absence_provable = value.absence_provable;
        op->result = {value.operation_id,
                      absl::FailedPreconditionError("operation is permanently UNKNOWN after an incomplete reconcile; "
                                                    "resend is refused"),
                      MemoryOutcome::Unknown,
                      {},
                      {},
                      Durability::Unspecified};
        ++core.unresolved;
        core.operations[{record.key, value.operation_id}] = op;
    }
    for(const auto& id: checkpoint.unresolved_operations)
    {
        if(core.operations.contains({record.key, id}))
            continue;
        auto op = std::make_shared<Operation>();
        op->conversation = record.session_id;
        op->dispatched_by = checkpoint.writer;
        op->result = {id, absl::OkStatus(), MemoryOutcome::Unknown, {}, {}, Durability::Unspecified};
        ++core.unresolved;
        core.operations[{record.key, id}] = op;
    }
    if(record.writer)
        return absl::OkStatus();
    if(!record.transition)
        record.transition = checkpoint.recovery;
    if(!record.acquisition_receipt && checkpoint.acquisition)
        record.acquisition_receipt = checkpoint.acquisition->acquisition_record_receipt_hlc;
    if(!record.last_own_receipt)
        record.last_own_receipt = checkpoint.last_own_receipt_hlc;
    if(!record.ownership)
        record.ownership = checkpoint.acquisition;
    record.reconcile_attempted |= checkpoint.reconcile_attempted;
    if(checkpoint.prior_state_unknown_below)
        record.prior_state_unknown_below = checkpoint.prior_state_unknown_below;
    // C1: recovery CASes the newest recorded incarnation and never adopts or writes into it.
    std::vector<WriterStamp> recorded;
    if(record.stamp)
        recorded.push_back(*record.stamp);
    if(checkpoint.writer)
        recorded.push_back(*checkpoint.writer);
    if(checkpoint.recovery)
        for(const auto& value: checkpoint.recovery->recovered_incarnations) recorded.push_back(value.writer);
    for(const auto& value: recorded)
        if(!record.stamp || value.incarnation > record.stamp->incarnation)
            record.stamp = value;
    return absl::OkStatus();
}
} // namespace detail
absl::StatusOr<std::string> encodeCheckpoint(const Checkpoint& checkpoint, size_t max_bytes)
{
    Json unknown = Json::array();
    for(const auto& value: checkpoint.permanently_unknown_operations)
    {
        Json incarnations = Json::array();
        for(const auto& writer: value.incarnations) incarnations.push_back(stamp(writer));
        unknown.push_back({{"id", value.operation_id},
                           {"incarnations", incarnations},
                           {"window", optional(value.window, range)},
                           {"absence_provable", value.absence_provable},
                           {"digest", optional(value.normalized_digest, hex)}});
    }
    Json dispositions = Json::array();
    for(const auto& value: checkpoint.dispositions)
    {
        if(value.result.outcome == ReconcileOutcome::Unknown)
            return absl::InvalidArgumentError("a checkpoint persists only LANDED or ABSENT dispositions");
        dispositions.push_back({{"id", value.result.operation_id},
                                {"outcome", value.result.outcome == ReconcileOutcome::Landed ? "landed" : "absent"},
                                {"landed", optional(value.result.landed, position)},
                                {"durability", static_cast<int>(value.result.observed_durability)},
                                {"prior", stamp(value.prior_writer)},
                                {"digest", optional(value.normalized_digest, hex)}});
    }
    Json acquisition;
    if(checkpoint.acquisition)
        acquisition = {{"host", checkpoint.acquisition->host_id},
                       {"lock", checkpoint.acquisition->launcher_lock_id},
                       {"expected",
                        optional(checkpoint.acquisition->expected_prior_incarnation,
                                 [](uint64_t value) { return Json(value); })},
                       {"receipt", optional(checkpoint.acquisition->acquisition_record_receipt_hlc, hlc)}};
    Json recovery;
    if(checkpoint.recovery)
    {
        Json recovered = Json::array();
        for(const auto& value: checkpoint.recovery->recovered_incarnations)
            recovered.push_back(
                    {stamp(value.writer), value.marker_operation_id ? Json(*value.marker_operation_id) : Json()});
        recovery = {{"transition", checkpoint.recovery->transition_id},
                    {"lower_bound", hlc(checkpoint.recovery->lower_bound)},
                    {"recovered", recovered},
                    {"marker",
                     optional(checkpoint.recovery->current_marker_operation_id,
                              [](const std::string& value) { return Json(value); })},
                    {"marker_hlc", optional(checkpoint.recovery->marker_hlc, hlc)}};
    }
    Json encoded = {{"v", checkpoint_version},
                    {"identity",
                     checkpoint.identity.control
                             ? Json::array({checkpoint.identity.agent_id, checkpoint.identity.slot, "control"})
                             : Json::array({checkpoint.identity.agent_id, checkpoint.identity.slot})},
                    {"context", {checkpoint.context.story_id, checkpoint.context.chronicle, checkpoint.context.name}},
                    {"causal_floor", hlc(checkpoint.causal_floor)},
                    {"processed_after", optional(checkpoint.processed_after, position)},
                    {"writer", optional(checkpoint.writer, stamp)},
                    {"acquisition", acquisition},
                    {"last_own_receipt", optional(checkpoint.last_own_receipt_hlc, hlc)},
                    {"recovery", recovery},
                    {"unresolved", checkpoint.unresolved_operations},
                    {"permanently_unknown", unknown},
                    {"dispositions", dispositions},
                    {"prior_state_unknown_below", optional(checkpoint.prior_state_unknown_below, hlc)},
                    {"acquisition_closed", checkpoint.acquisition_closed},
                    {"reconcile_attempted", checkpoint.reconcile_attempted},
                    {"takeover_required", checkpoint.takeover_required}};
    std::string result;
    try
    {
        result = encoded.dump();
    }
    catch(const Json::exception&)
    {
        return absl::InvalidArgumentError("checkpoint strings must be UTF-8");
    }
    if(result.size() > max_bytes)
        return absl::ResourceExhaustedError("encoded checkpoint exceeds max_bytes");
    return result;
}
absl::StatusOr<Checkpoint> decodeCheckpoint(std::string_view bytes)
{
    const auto encoded = Json::parse(bytes, nullptr, false);
    if(!encoded.is_object() || !encoded.contains("v") || encoded.at("v") != checkpoint_version)
        return absl::InvalidArgumentError("not a chronolog-context-checkpoint/v1 value");
    try
    {
        Checkpoint result;
        const auto& identity = encoded.at("identity");
        if(identity.size() > 3 || (identity.size() == 3 && identity.at(2) != "control"))
            return absl::InvalidArgumentError("unknown checkpoint identity kind");
        result.identity = {text(identity.at(0)), text(identity.at(1)), identity.size() == 3};
        const auto& context = encoded.at("context");
        result.context = {number<uint64_t>(context.at(0)), text(context.at(1)), text(context.at(2))};
        result.causal_floor = toHlc(encoded.at("causal_floor"));
        result.processed_after = toOptional<Position>(encoded.at("processed_after"), toPosition);
        result.writer = toOptional<WriterStamp>(encoded.at("writer"), toStamp);
        if(const auto& acquisition = encoded.at("acquisition"); !acquisition.is_null())
            result.acquisition =
                    AcquisitionProvenance{text(acquisition.at("host")),
                                          text(acquisition.at("lock")),
                                          toOptional<uint64_t>(acquisition.at("expected"), number<uint64_t>),
                                          toOptional<Hlc>(acquisition.at("receipt"), toHlc)};
        result.last_own_receipt_hlc = toOptional<Hlc>(encoded.at("last_own_receipt"), toHlc);
        if(const auto& recovery = encoded.at("recovery"); !recovery.is_null())
        {
            ReconcileCheckpoint value;
            value.transition_id = text(recovery.at("transition"));
            value.lower_bound = toHlc(recovery.at("lower_bound"));
            for(const auto& recovered: recovery.at("recovered"))
                value.recovered_incarnations.push_back(
                        {toStamp(recovered.at(0)), toOptional<std::string>(recovered.at(1), text)});
            value.current_marker_operation_id = toOptional<std::string>(recovery.at("marker"), text);
            value.marker_hlc = toOptional<Hlc>(recovery.at("marker_hlc"), toHlc);
            result.recovery = std::move(value);
        }
        for(const auto& id: encoded.at("unresolved")) result.unresolved_operations.push_back(text(id));
        for(const auto& value: encoded.at("permanently_unknown"))
        {
            UnknownOperation unknown;
            unknown.operation_id = text(value.at("id"));
            for(const auto& writer: value.at("incarnations")) unknown.incarnations.push_back(toStamp(writer));
            unknown.window = toOptional<client::HlcRange>(value.at("window"), toRange);
            if(!value.at("absence_provable").is_boolean())
                throw std::invalid_argument("absence_provable");
            unknown.absence_provable = value.at("absence_provable").get<bool>();
            unknown.normalized_digest =
                    toOptional<std::string>(value.at("digest"), [](const Json& digest) { return unhex(text(digest)); });
            result.permanently_unknown_operations.push_back(std::move(unknown));
        }
        for(const auto& value: encoded.at("dispositions"))
        {
            OperationDisposition disposition;
            const auto outcome = text(value.at("outcome"));
            if(outcome != "landed" && outcome != "absent")
                throw std::invalid_argument("disposition outcome");
            const auto durability = number<int>(value.at("durability"));
            if(durability < 0 || durability > static_cast<int>(Durability::Durable))
                throw std::out_of_range("durability");
            disposition.result = {text(value.at("id")),
                                  outcome == "landed" ? ReconcileOutcome::Landed : ReconcileOutcome::Absent,
                                  toOptional<Position>(value.at("landed"), toPosition),
                                  static_cast<Durability>(durability)};
            disposition.prior_writer = toStamp(value.at("prior"));
            disposition.normalized_digest =
                    toOptional<std::string>(value.at("digest"), [](const Json& digest) { return unhex(text(digest)); });
            result.dispositions.push_back(std::move(disposition));
        }
        result.prior_state_unknown_below = toOptional<Hlc>(encoded.at("prior_state_unknown_below"), toHlc);
        for(const auto* flag: {"acquisition_closed", "reconcile_attempted", "takeover_required"})
            if(!encoded.at(flag).is_boolean())
                throw std::invalid_argument(flag);
        result.acquisition_closed = encoded.at("acquisition_closed").get<bool>();
        result.reconcile_attempted = encoded.at("reconcile_attempted").get<bool>();
        result.takeover_required = encoded.at("takeover_required").get<bool>();
        return result;
    }
    catch(const std::exception& error)
    {
        return absl::InvalidArgumentError(std::string("malformed checkpoint: ") + error.what());
    }
}
} // namespace chronolog::context
