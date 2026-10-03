#include "binding.h"

namespace binding
{
namespace
{
template <class E>
const char* name(E value, std::initializer_list<const char*> names)
{
    auto index = static_cast<size_t>(value);
    return index < names.size() ? names.begin()[index] : "UNKNOWN";
}
template <class E>
E parse(Js value, std::initializer_list<const char*> names)
{
    auto input = text(value);
    for(size_t i = 0; i < names.size(); ++i)
        if(input == names.begin()[i])
            return static_cast<E>(i);
    throw Napi::RangeError::New(value.Env(), "unknown enum value " + input);
}
const std::initializer_list<const char*> session_states = {"READY",
                                                           "TRANSPORT_PENDING",
                                                           "NEEDS_RECONCILE",
                                                           "FENCED",
                                                           "CLOSED"};
const std::initializer_list<const char*> memory_outcomes =
        {"DURABLE", "RAM_ONLY_MAY_VANISH", "REJECTED", "UNKNOWN", "FENCED", "LANDED"};
const std::initializer_list<const char*> delivery_limits = {"NONE", "EVENTS", "BYTES", "OVERSIZED_EVENT", "READ_CALLS"};
const std::initializer_list<const char*> reconcile_outcomes = {"LANDED", "ABSENT", "UNKNOWN"};
const std::initializer_list<const char*> follow_starts = {"NOW", "BEGINNING", "POSITION"};
const std::initializer_list<const char*> accesses = {"READ_ONLY", "READ_WRITE"};

Js big(Napi::Env env, uint64_t value) { return Napi::BigInt::New(env, value); }
Js count(Napi::Env env, size_t value) { return Napi::Number::New(env, static_cast<double>(value)); }
Js strings(Napi::Env env, const std::vector<std::string>& values) { return array(env, values); }
size_t size(Js value) { return static_cast<size_t>(number(value, max_safe_integer)); }
bool flag(Js value)
{
    if(!value.IsBoolean())
        throw Napi::TypeError::New(value.Env(), "expected a boolean");
    return value.As<Napi::Boolean>().Value();
}
template <class T, class F>
std::optional<T> optional(Napi::Object input, const char* key, F convert)
{
    if(!has(input, key))
        return std::nullopt;
    return convert(input.Get(key));
}
template <class T, class F>
std::vector<T> list(Js value, F convert)
{
    if(!value.IsArray())
        throw Napi::TypeError::New(value.Env(), "expected an array");
    auto input = value.As<Napi::Array>();
    std::vector<T> out;
    out.reserve(input.Length());
    for(uint32_t i = 0; i < input.Length(); ++i) out.push_back(convert(input.Get(i)));
    return out;
}
template <class T, class F>
Js jsList(Napi::Env env, const std::vector<T>& values, F convert)
{
    auto out = Napi::Array::New(env, values.size());
    for(size_t i = 0; i < values.size(); ++i) out.Set(i, convert(env, values[i]));
    return out;
}

Js ref(Napi::Env env, const ctx::ContextRef& value)
{
    auto out = Napi::Object::New(env);
    out.Set("storyId", big(env, value.story_id));
    out.Set("chronicle", value.chronicle);
    out.Set("name", value.name);
    return out;
}
ctx::ContextRef ref(Js value)
{
    auto input = object(value);
    return {unsigned64(input.Get("storyId")), text(input.Get("chronicle")), text(input.Get("name"))};
}
Js identity(Napi::Env env, const ctx::AgentIdentity& value)
{
    auto out = Napi::Object::New(env);
    out.Set("agentId", value.agent_id);
    out.Set("slot", value.slot);
    return out;
}
ctx::AgentIdentity identity(Js value)
{
    auto input = object(value);
    return {text(input.Get("agentId")), text(input.Get("slot"))};
}
Js stamp(Napi::Env env, const ctx::WriterStamp& value)
{
    auto out = Napi::Object::New(env);
    out.Set("writerId", big(env, value.writer_id));
    out.Set("incarnation", big(env, value.incarnation));
    return out;
}
ctx::WriterStamp stamp(Js value)
{
    auto input = object(value);
    return {unsigned64(input.Get("writerId")), unsigned64(input.Get("incarnation"))};
}
void optionalStamp(Napi::Object out, const char* key, const std::optional<ctx::WriterStamp>& value)
{
    if(value)
        out.Set(key, stamp(out.Env(), *value));
}
Js page(Napi::Env env, const ctx::Page& value)
{
    auto out = Napi::Object::New(env);
    out.Set("events", array(env, value.events));
    set(out, "range", value.range);
    set(out, "completion", value.completion);
    set(out, "completionRange", value.completion_range);
    out.Set("streamStatus", status(env, value.stream_status));
    out.Set("limited", name(value.limited, delivery_limits));
    out.Set("rawBytes", count(env, value.raw_bytes));
    out.Set("answerComplete", value.answer_complete);
    out.Set("hasMore", value.has_more);
    out.Set("idle", value.idle);
    out.Set("cutCoversCausalFloor", value.cut_covers_causal_floor);
    set(out, "after", value.after);
    set(out, "nextCursor", value.next_cursor);
    set(out, "deliveredPrefixEnd", value.delivered_prefix_end);
    return out;
}
Js prior(Napi::Env env, const ctx::PriorOutcome& value)
{
    auto out = Napi::Object::New(env);
    out.Set("operationId", value.operation_id);
    out.Set("status", status(env, value.status));
    out.Set("outcome", name(value.outcome, memory_outcomes));
    set(out, "receipt", value.receipt);
    set(out, "landed", value.landed);
    out.Set("observedDurability", static_cast<int>(value.observed_durability));
    return out;
}
Js reconciledJs(Napi::Env env, const ctx::ReconciledOperation& value)
{
    auto out = Napi::Object::New(env);
    out.Set("operationId", value.operation_id);
    out.Set("outcome", name(value.outcome, reconcile_outcomes));
    set(out, "landed", value.landed);
    out.Set("observedDurability", static_cast<int>(value.observed_durability));
    return out;
}
ctx::ReconciledOperation reconciled(Js value)
{
    auto input = object(value);
    ctx::ReconciledOperation out{text(input.Get("operationId")),
                                 parse<ctx::ReconcileOutcome>(input.Get("outcome"), reconcile_outcomes),
                                 optional<sdk::Position>(input, "landed", position)};
    if(has(input, "observedDurability"))
        out.observed_durability = static_cast<Durability>(static_cast<int>(number(input.Get("observedDurability"), 2)));
    return out;
}
Js checkpoint(Napi::Env env, const ctx::Checkpoint& value)
{
    auto out = Napi::Object::New(env);
    out.Set("identity", identity(env, value.identity));
    out.Set("context", ref(env, value.context));
    out.Set("causalFloor", js(env, value.causal_floor));
    set(out, "processedAfter", value.processed_after);
    optionalStamp(out, "writer", value.writer);
    if(value.acquisition)
    {
        auto acquisition = Napi::Object::New(env);
        acquisition.Set("hostId", value.acquisition->host_id);
        acquisition.Set("launcherLockId", value.acquisition->launcher_lock_id);
        if(value.acquisition->expected_prior_incarnation)
            acquisition.Set("expectedPriorIncarnation", big(env, *value.acquisition->expected_prior_incarnation));
        set(acquisition, "acquisitionRecordReceiptHlc", value.acquisition->acquisition_record_receipt_hlc);
        out.Set("acquisition", acquisition);
    }
    set(out, "lastOwnReceiptHlc", value.last_own_receipt_hlc);
    if(value.recovery)
    {
        auto recovery = Napi::Object::New(env);
        recovery.Set("transitionId", value.recovery->transition_id);
        recovery.Set("lowerBound", js(env, value.recovery->lower_bound));
        recovery.Set("recoveredIncarnations",
                     jsList(env,
                            value.recovery->recovered_incarnations,
                            [](Napi::Env env, const ctx::RecoveryIncarnation& item)
                            {
                                auto entry = Napi::Object::New(env);
                                entry.Set("writer", stamp(env, item.writer));
                                set(entry, "markerOperationId", item.marker_operation_id);
                                return entry;
                            }));
        set(recovery, "currentMarkerOperationId", value.recovery->current_marker_operation_id);
        set(recovery, "markerHlc", value.recovery->marker_hlc);
        out.Set("recovery", recovery);
    }
    out.Set("unresolvedOperations", strings(env, value.unresolved_operations));
    out.Set("permanentlyUnknownOperations",
            jsList(env,
                   value.permanently_unknown_operations,
                   [](Napi::Env env, const ctx::UnknownOperation& item)
                   {
                       auto entry = Napi::Object::New(env);
                       entry.Set("operationId", item.operation_id);
                       entry.Set("incarnations",
                                 jsList(env,
                                        item.incarnations,
                                        [](Napi::Env env, const auto& s) { return stamp(env, s); }));
                       set(entry, "window", item.window);
                       entry.Set("absenceProvable", item.absence_provable);
                       if(item.normalized_digest)
                           entry.Set("normalizedDigest", data(env, *item.normalized_digest));
                       return entry;
                   }));
    out.Set("dispositions",
            jsList(env,
                   value.dispositions,
                   [](Napi::Env env, const ctx::OperationDisposition& item)
                   {
                       auto entry = Napi::Object::New(env);
                       entry.Set("result", reconciledJs(env, item.result));
                       entry.Set("priorWriter", stamp(env, item.prior_writer));
                       if(item.normalized_digest)
                           entry.Set("normalizedDigest", data(env, *item.normalized_digest));
                       return entry;
                   }));
    set(out, "priorStateUnknownBelow", value.prior_state_unknown_below);
    out.Set("acquisitionClosed", value.acquisition_closed);
    out.Set("reconcileAttempted", value.reconcile_attempted);
    out.Set("takeoverRequired", value.takeover_required);
    return out;
}
ctx::AcquisitionProvenance provenance(Js value)
{
    auto input = object(value);
    return {text(input.Get("hostId")),
            text(input.Get("launcherLockId")),
            optional<uint64_t>(input, "expectedPriorIncarnation", unsigned64),
            optional<Hlc>(input, "acquisitionRecordReceiptHlc", hlc)};
}
ctx::Checkpoint checkpoint(Js value)
{
    auto input = object(value);
    ctx::Checkpoint out;
    out.identity = identity(input.Get("identity"));
    out.context = ref(input.Get("context"));
    out.causal_floor = hlc(input.Get("causalFloor"));
    out.processed_after = optional<sdk::Position>(input, "processedAfter", position);
    out.writer = optional<ctx::WriterStamp>(input, "writer", [](Js v) { return stamp(v); });
    out.acquisition = optional<ctx::AcquisitionProvenance>(input, "acquisition", provenance);
    out.last_own_receipt_hlc = optional<Hlc>(input, "lastOwnReceiptHlc", hlc);
    if(has(input, "recovery"))
    {
        auto recovery = object(input.Get("recovery"));
        ctx::ReconcileCheckpoint item;
        item.transition_id = text(recovery.Get("transitionId"));
        item.lower_bound = hlc(recovery.Get("lowerBound"));
        item.recovered_incarnations = list<ctx::RecoveryIncarnation>(
                recovery.Get("recoveredIncarnations"),
                [](Js v)
                {
                    auto entry = object(v);
                    return ctx::RecoveryIncarnation{stamp(entry.Get("writer")),
                                                    optional<std::string>(entry, "markerOperationId", text)};
                });
        item.current_marker_operation_id = optional<std::string>(recovery, "currentMarkerOperationId", text);
        item.marker_hlc = optional<Hlc>(recovery, "markerHlc", hlc);
        out.recovery = std::move(item);
    }
    if(has(input, "unresolvedOperations"))
        out.unresolved_operations = list<std::string>(input.Get("unresolvedOperations"), text);
    if(has(input, "permanentlyUnknownOperations"))
        out.permanently_unknown_operations = list<ctx::UnknownOperation>(
                input.Get("permanentlyUnknownOperations"),
                [](Js v)
                {
                    auto entry = object(v);
                    ctx::UnknownOperation item;
                    item.operation_id = text(entry.Get("operationId"));
                    item.incarnations =
                            list<ctx::WriterStamp>(entry.Get("incarnations"), [](Js s) { return stamp(s); });
                    item.window = optional<sdk::HlcRange>(entry, "window", hlcRange);
                    item.absence_provable = has(entry, "absenceProvable") && flag(entry.Get("absenceProvable"));
                    item.normalized_digest = optional<std::string>(entry, "normalizedDigest", bytes);
                    return item;
                });
    if(has(input, "dispositions"))
        out.dispositions = list<ctx::OperationDisposition>(
                input.Get("dispositions"),
                [](Js v)
                {
                    auto entry = object(v);
                    return ctx::OperationDisposition{reconciled(entry.Get("result")),
                                                     stamp(entry.Get("priorWriter")),
                                                     optional<std::string>(entry, "normalizedDigest", bytes)};
                });
    out.prior_state_unknown_below = optional<Hlc>(input, "priorStateUnknownBelow", hlc);
    out.acquisition_closed = has(input, "acquisitionClosed") && flag(input.Get("acquisitionClosed"));
    out.reconcile_attempted = has(input, "reconcileAttempted") && flag(input.Get("reconcileAttempted"));
    out.takeover_required = has(input, "takeoverRequired") && flag(input.Get("takeoverRequired"));
    return out;
}
ctx::PageLimits limits(Napi::Object options)
{
    ctx::PageLimits out;
    if(!has(options, "limits"))
        return out;
    auto input = object(options.Get("limits"));
    if(has(input, "maxEvents"))
        out.max_events = size(input.Get("maxEvents"));
    if(has(input, "maxRawBytes"))
        out.max_raw_bytes = size(input.Get("maxRawBytes"));
    return out;
}
Js throwStatus(Napi::Env env, const absl::Status& value) { throw Napi::Error(env, error(env, value)); }
} // namespace

// Converters the shared Work and thread templates resolve by overload.
Js js(Napi::Env env, const ctx::ContextRef& value) { return ref(env, value); }
Js js(Napi::Env env, const ctx::MemoryResult& value)
{
    auto out = Napi::Object::New(env);
    out.Set("current", prior(env, value.current));
    out.Set("resolvedPrior", jsList(env, value.resolved_prior, prior));
    set(out, "blockingOperationId", value.blocking_operation_id);
    out.Set("state", name(value.state, session_states));
    return out;
}
Js js(Napi::Env env, const ctx::Page& value) { return page(env, value); }
Js js(Napi::Env env, const ctx::LatestResult& value)
{
    auto out = Napi::Object::New(env);
    out.Set("page", page(env, value.page));
    set(out, "asOf", value.as_of);
    out.Set("selectionComplete", value.selection_complete);
    return out;
}
Js js(Napi::Env env, const ctx::ReconcileResult& value)
{
    auto out = Napi::Object::New(env);
    optionalStamp(out, "writer", value.writer);
    set(out, "markerHlc", value.marker_hlc);
    set(out, "range", value.range);
    out.Set("operations", jsList(env, value.operations, reconciledJs));
    set(out, "completion", value.completion);
    out.Set("status", status(env, value.status));
    out.Set("attempted", value.attempted);
    out.Set("permanentlyUnknownOperations", strings(env, value.permanently_unknown_operations));
    out.Set("proofComplete", value.proof_complete);
    out.Set("supplyAllUnseenOperationIds", value.supply_all_unseen_operation_ids);
    out.Set("omittedOperationIdsMayDuplicate", value.omitted_operation_ids_may_duplicate);
    return out;
}
Js js(Napi::Env env, const ctx::CloseResult& value)
{
    auto out = Napi::Object::New(env);
    out.Set("releaseCommitted", value.release_committed);
    out.Set("fenced", value.fenced);
    return out;
}
Js js(Napi::Env env, const ctx::FollowResult& value)
{
    auto out = Napi::Object::New(env);
    out.Set("pages",
            jsList(env,
                   value.pages,
                   [](Napi::Env env, const ctx::FollowPage& item)
                   {
                       auto entry = Napi::Object::New(env);
                       entry.Set("context", ref(env, item.context));
                       entry.Set("page", page(env, item.page));
                       set(entry, "resume", item.resume);
                       set(entry, "startingCut", item.starting_cut);
                       entry.Set("uncertifiedRouteKeepers", strings(env, item.uncertified_route_keepers));
                       return entry;
                   }));
    out.Set("status", status(env, value.status));
    out.Set("idle", value.idle);
    return out;
}

namespace
{
Js connectContext(const Napi::CallbackInfo& info)
{
    auto options = object(info[0]);
    ctx::ContextOptions config;
    config.sdk = clientOptions(options);
    if(has(options, "maxWritableSessions"))
        config.max_writable_sessions = size(options.Get("maxWritableSessions"));
    if(has(options, "maxPendingOperationBytes"))
        config.max_pending_operation_bytes = size(options.Get("maxPendingOperationBytes"));
    if(has(options, "maxCompletedOperations"))
        config.max_completed_operations = size(options.Get("maxCompletedOperations"));
    if(has(options, "maxUnresolvedOperations"))
        config.max_unresolved_operations = size(options.Get("maxUnresolvedOperations"));
    if(has(options, "maxPersistedDispositions"))
        config.max_persisted_dispositions = size(options.Get("maxPersistedDispositions"));
    if(has(options, "maxCheckpointPayloadBytes"))
        config.max_checkpoint_payload_bytes = size(options.Get("maxCheckpointPayloadBytes"));
    if(has(options, "cutProbeWidthNs"))
        config.cut_probe_width = std::chrono::nanoseconds(signed64(options.Get("cutProbeWidthNs")));
    auto end = deadline(object(info[1]));
    return work<Held>(info.Env(),
                      [config = std::move(config), end]() mutable -> absl::StatusOr<Held>
                      {
                          auto result = ctx::ContextClient::Connect(std::move(config), end);
                          if(!result.ok())
                              return result.status();
                          auto out = std::make_shared<Handle>(Handle::ContextClient);
                          out->context = std::make_shared<ctx::ContextClient>(std::move(*result));
                          return out;
                      });
}
Js ensureContext(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::ContextClient);
    auto chronicle = text(info[1]), label = text(info[2]);
    auto end = deadline(object(info[3]));
    return work<ctx::ContextRef>(info.Env(),
                                 [held, chronicle, label, end]
                                 { return held->context->ensureContext(chronicle, label, end); });
}
Js listContexts(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::ContextClient);
    auto chronicle = text(info[1]);
    auto end = deadline(object(info[2]));
    return work<std::vector<ctx::ContextRef>>(info.Env(),
                                              [held, chronicle, end]
                                              { return held->context->listContexts(chronicle, end); });
}
Js open(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::ContextClient);
    auto context = ref(info[1]);
    auto agent = identity(info[2]);
    auto input = object(info[3]);
    ctx::OpenOptions options;
    if(has(input, "access"))
        options.access = parse<ctx::Access>(input.Get("access"), accesses);
    if(has(input, "sessionId"))
        options.session_id = text(input.Get("sessionId"));
    options.resume = optional<ctx::Checkpoint>(input, "resume", [](Js v) { return checkpoint(v); });
    options.ownership = optional<ctx::AcquisitionProvenance>(input, "ownership", provenance);
    auto end = deadline(object(info[4]));
    return work<Held>(info.Env(),
                      [held, context, agent, options = std::move(options), end]() mutable -> absl::StatusOr<Held>
                      {
                          auto session = held->context->open(std::move(context), agent, std::move(options), end);
                          if(!session.ok())
                              return session.status();
                          auto out = std::make_shared<Handle>(Handle::Session);
                          out->session = std::move(*session);
                          return out;
                      });
}
Js remember(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    auto input = object(info[1]);
    ctx::Memory memory;
    memory.operation_id = text(input.Get("operationId"));
    auto envelope_input = object(input.Get("envelope"));
    memory.envelope = envelope(envelope_input.Get("payload"), envelope_input);
    if(has(input, "durability"))
    {
        auto level = number(input.Get("durability"), 2);
        memory.durability = static_cast<Durability>(static_cast<int>(level));
    }
    memory.physical = optional<TimeReading>(input, "physical", timeReading);
    auto options = object(info[2]);
    ctx::RememberOptions remember_options;
    if(has(options, "resendAfterAbsent"))
        remember_options.resend_after_absent = flag(options.Get("resendAfterAbsent"));
    auto end = deadline(object(info[3]));
    return work<ctx::MemoryResult>(info.Env(),
                                   [held, memory = std::move(memory), remember_options, end]
                                   { return held->session->remember(memory, remember_options, end); });
}
Js recall(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    auto input = object(info[1]);
    ctx::RecallOptions options;
    options.start = optional<Hlc>(input, "start", hlc);
    options.end = optional<Hlc>(input, "end", hlc);
    options.cursor = optional<std::string>(input, "cursor", text);
    options.limits = limits(input);
    if(has(input, "maxReadCalls"))
        options.max_read_calls = size(input.Get("maxReadCalls"));
    auto end = deadline(object(info[2]));
    return work<ctx::Page>(info.Env(),
                           [held, options = std::move(options), end] { return held->session->recall(options, end); });
}
Js latest(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    auto n = size(info[1]);
    auto input = object(info[2]);
    ctx::LatestOptions options;
    options.before = optional<Hlc>(input, "before", hlc);
    options.limits = limits(input);
    if(has(input, "maxReadCalls"))
        options.max_read_calls = size(input.Get("maxReadCalls"));
    auto end = deadline(object(info[3]));
    return work<ctx::LatestResult>(info.Env(),
                                   [held, n, options = std::move(options), end]
                                   { return held->session->latest(n, options, end); });
}
Js reconcile(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    auto input = object(info[1]);
    ctx::ReconcileOptions options;
    if(has(input, "operationIds"))
        options.operation_ids = list<std::string>(input.Get("operationIds"), text);
    if(has(input, "takeover"))
        options.takeover = flag(input.Get("takeover"));
    if(has(input, "maxReadCalls"))
        options.max_read_calls = size(input.Get("maxReadCalls"));
    auto end = deadline(object(info[2]));
    return work<ctx::ReconcileResult>(info.Env(),
                                      [held, options = std::move(options), end]
                                      { return held->session->reconcile(options, end); });
}
Js acknowledgeProcessed(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    if(auto result = held->session->acknowledgeProcessed(position(info[1])); !result.ok())
        return throwStatus(info.Env(), result);
    return info.Env().Undefined();
}
Js sessionCheckpoint(const Napi::CallbackInfo& info)
{
    return checkpoint(info.Env(), handle(info[0], Handle::Session)->session->checkpoint());
}
Js sessionStatus(const Napi::CallbackInfo& info)
{
    auto env = info.Env();
    auto value = handle(info[0], Handle::Session)->session->status();
    auto out = Napi::Object::New(env);
    out.Set("context", ref(env, value.context));
    out.Set("identity", identity(env, value.identity));
    optionalStamp(out, "writer", value.writer);
    out.Set("state", name(value.state, session_states));
    set(out, "blockingOperationId", value.blocking_operation_id);
    out.Set("unresolvedOperations", strings(env, value.unresolved_operations));
    out.Set("permanentlyUnknownOperations", strings(env, value.permanently_unknown_operations));
    out.Set("reconcileAttempted", value.reconcile_attempted);
    out.Set("causalFloor", js(env, value.causal_floor));
    return out;
}
Js close(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::Session);
    auto end = deadline(object(info[1]));
    return work<ctx::CloseResult>(info.Env(), [held, end] { return held->session->close(end); });
}
Js follow(const Napi::CallbackInfo& info)
{
    auto held = handle(info[0], Handle::ContextClient);
    auto inputs = list<ctx::FollowInput>(info[1],
                                         [](Js v)
                                         {
                                             auto entry = object(v);
                                             ctx::FollowInput item;
                                             item.session = handle(entry.Get("session"), Handle::Session)->session;
                                             if(has(entry, "from"))
                                                 item.from = parse<ctx::FollowFrom>(entry.Get("from"), follow_starts);
                                             item.after = optional<sdk::Position>(entry, "after", position);
                                             return item;
                                         });
    auto input = object(info[2]);
    ctx::FollowOptions options;
    options.limits = limits(input);
    if(has(input, "waitMs"))
    {
        // The native wait has whole-second granularity; a partial second rounds up.
        auto wait = static_cast<int64_t>(number(input.Get("waitMs"), 60000));
        options.wait = std::chrono::seconds((wait + 999) / 1000);
    }
    auto end = deadline(object(info[3]));
    return thread<ctx::FollowResult>(info.Env(),
                                     [held, inputs = std::move(inputs), options, end]
                                     { return held->context->follow(inputs, options, end); });
}
Js encodeCheckpoint(const Napi::CallbackInfo& info)
{
    auto value = checkpoint(info[0]);
    auto encoded = info[1].IsUndefined() ? ctx::encodeCheckpoint(value) : ctx::encodeCheckpoint(value, size(info[1]));
    if(!encoded.ok())
        return throwStatus(info.Env(), encoded.status());
    return data(info.Env(), *encoded);
}
Js decodeCheckpoint(const Napi::CallbackInfo& info)
{
    auto decoded = ctx::decodeCheckpoint(bytes(info[0]));
    if(!decoded.ok())
        return throwStatus(info.Env(), decoded.status());
    return checkpoint(info.Env(), *decoded);
}
} // namespace
void initContext(Napi::Env env, Napi::Object exports)
{
    exports.Set("connectContext", Napi::Function::New(env, connectContext));
    exports.Set("ensureContext", Napi::Function::New(env, ensureContext));
    exports.Set("listContexts", Napi::Function::New(env, listContexts));
    exports.Set("open", Napi::Function::New(env, open));
    exports.Set("remember", Napi::Function::New(env, remember));
    exports.Set("recall", Napi::Function::New(env, recall));
    exports.Set("latest", Napi::Function::New(env, latest));
    exports.Set("reconcile", Napi::Function::New(env, reconcile));
    exports.Set("acknowledgeProcessed", Napi::Function::New(env, acknowledgeProcessed));
    exports.Set("checkpoint", Napi::Function::New(env, sessionCheckpoint));
    exports.Set("sessionStatus", Napi::Function::New(env, sessionStatus));
    exports.Set("close", Napi::Function::New(env, close));
    exports.Set("follow", Napi::Function::New(env, follow));
    exports.Set("encodeCheckpoint", Napi::Function::New(env, encodeCheckpoint));
    exports.Set("decodeCheckpoint", Napi::Function::New(env, decodeCheckpoint));
}
} // namespace binding
