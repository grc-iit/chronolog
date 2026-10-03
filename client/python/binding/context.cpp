#include "binding.h"

namespace binding
{
namespace
{
using ContextClient = Held<ctx::ContextClient>;
using Session = Held<ctx::ContextSession>;
using binding::pack;
nb::object pack(const ctx::ContextRef&);
nb::object pack(const ctx::AgentIdentity&);
nb::object pack(const ctx::WriterStamp&);
nb::object pack(const ctx::Page&);
nb::object pack(const ctx::PriorOutcome&);
nb::object pack(const ctx::ReconciledOperation&);
nb::object pack(const ctx::AcquisitionProvenance&);

nb::int_ enumValue(auto value) { return nb::int_(static_cast<int>(value)); }
size_t size(nb::handle h) { return static_cast<size_t>(u64(h)); }
nb::object data(const std::string& value) { return nb::bytes(value.data(), value.size()); }
nb::tuple strings(const std::vector<std::string>& values)
{
    return tuple(values, [](const std::string& s) { return nb::str(s.data(), s.size()); });
}

nb::object pack(const ctx::ContextRef& r)
{
    return value("ContextRef", fields("story_id"_a = r.story_id, "chronicle"_a = r.chronicle, "name"_a = r.name));
}
ctx::ContextRef ref(nb::handle h) { return {u64(h.attr("story_id")), text(h.attr("chronicle")), text(h.attr("name"))}; }
nb::object pack(const ctx::AgentIdentity& i)
{
    return value("AgentIdentity", fields("agent_id"_a = i.agent_id, "slot"_a = i.slot, "control"_a = i.control));
}
ctx::AgentIdentity identity(nb::handle h)
{
    return {text(h.attr("agent_id")), text(h.attr("slot")), nb::cast<bool>(h.attr("control"))};
}
nb::object pack(const ctx::WriterStamp& s)
{
    return value("WriterStamp", fields("writer_id"_a = s.writer_id, "incarnation"_a = s.incarnation));
}
ctx::WriterStamp stamp(nb::handle h) { return {u64(h.attr("writer_id")), u64(h.attr("incarnation"))}; }
template <class T>
nb::object packed(const std::optional<T>& input)
{
    return maybe(input, [](const T& item) { return pack(item); });
}

nb::object pack(const ctx::Page& p)
{
    return value("Page",
                 fields("events"_a = tuple(p.events, [](const Event& e) { return pack(e); }),
                        "range"_a = packed(p.range),
                        "completion"_a = packed(p.completion),
                        "completion_range"_a = packed(p.completion_range),
                        "stream_status"_a = status(p.stream_status),
                        "limited"_a = enumValue(p.limited),
                        "raw_bytes"_a = p.raw_bytes,
                        "answer_complete"_a = p.answer_complete,
                        "has_more"_a = p.has_more,
                        "idle"_a = p.idle,
                        "cut_covers_causal_floor"_a = p.cut_covers_causal_floor,
                        "after"_a = packed(p.after),
                        "next_cursor"_a = p.next_cursor,
                        "delivered_prefix_end"_a = packed(p.delivered_prefix_end)));
}
nb::object pack(const ctx::PriorOutcome& p)
{
    return value("PriorOutcome",
                 fields("operation_id"_a = p.operation_id,
                        "status"_a = status(p.status),
                        "outcome"_a = enumValue(p.outcome),
                        "receipt"_a = packed(p.receipt),
                        "landed"_a = packed(p.landed),
                        "observed_durability"_a = enumValue(p.observed_durability)));
}
nb::object pack(const ctx::ReconciledOperation& r)
{
    return value("ReconciledOperation",
                 fields("operation_id"_a = r.operation_id,
                        "outcome"_a = enumValue(r.outcome),
                        "landed"_a = packed(r.landed),
                        "observed_durability"_a = enumValue(r.observed_durability)));
}
ctx::ReconciledOperation reconciled(nb::handle h)
{
    return {text(h.attr("operation_id")),
            static_cast<ctx::ReconcileOutcome>(nb::cast<int>(h.attr("outcome"))),
            optional<sdk::Position>(h.attr("landed"), position),
            static_cast<Durability>(nb::cast<int>(h.attr("observed_durability")))};
}
nb::object pack(const ctx::AcquisitionProvenance& a)
{
    return value("AcquisitionProvenance",
                 fields("host_id"_a = a.host_id,
                        "launcher_lock_id"_a = a.launcher_lock_id,
                        "expected_prior_incarnation"_a = a.expected_prior_incarnation,
                        "acquisition_record_receipt_hlc"_a = packed(a.acquisition_record_receipt_hlc)));
}
ctx::AcquisitionProvenance provenance(nb::handle h)
{
    return {text(h.attr("host_id")),
            text(h.attr("launcher_lock_id")),
            optional<uint64_t>(h.attr("expected_prior_incarnation"), u64),
            optional<Hlc>(h.attr("acquisition_record_receipt_hlc"), hlc)};
}
nb::object digest(const std::optional<std::string>& value) { return maybe(value, data); }

nb::object pack(const ctx::Checkpoint& c)
{
    nb::object recovery = nb::none();
    if(c.recovery)
    {
        const auto& r = *c.recovery;
        recovery = value("ReconcileCheckpoint",
                         fields("transition_id"_a = r.transition_id,
                                "lower_bound"_a = pack(r.lower_bound),
                                "recovered_incarnations"_a =
                                        tuple(r.recovered_incarnations,
                                              [](const ctx::RecoveryIncarnation& i)
                                              {
                                                  return value("RecoveryIncarnation",
                                                               fields("writer"_a = pack(i.writer),
                                                                      "marker_operation_id"_a = i.marker_operation_id));
                                              }),
                                "current_marker_operation_id"_a = r.current_marker_operation_id,
                                "marker_hlc"_a = packed(r.marker_hlc)));
    }
    auto unknown = [](const ctx::UnknownOperation& u)
    {
        return value("UnknownOperation",
                     fields("operation_id"_a = u.operation_id,
                            "incarnations"_a = tuple(u.incarnations, [](const auto& s) { return pack(s); }),
                            "window"_a = packed(u.window),
                            "absence_provable"_a = u.absence_provable,
                            "normalized_digest"_a = digest(u.normalized_digest)));
    };
    auto disposition = [](const ctx::OperationDisposition& d)
    {
        return value("OperationDisposition",
                     fields("result"_a = pack(d.result),
                            "prior_writer"_a = pack(d.prior_writer),
                            "normalized_digest"_a = digest(d.normalized_digest)));
    };
    return value("Checkpoint",
                 fields("identity"_a = pack(c.identity),
                        "context"_a = pack(c.context),
                        "causal_floor"_a = pack(c.causal_floor),
                        "processed_after"_a = packed(c.processed_after),
                        "writer"_a = packed(c.writer),
                        "acquisition"_a = packed(c.acquisition),
                        "last_own_receipt_hlc"_a = packed(c.last_own_receipt_hlc),
                        "recovery"_a = recovery,
                        "unresolved_operations"_a = strings(c.unresolved_operations),
                        "permanently_unknown_operations"_a = tuple(c.permanently_unknown_operations, unknown),
                        "dispositions"_a = tuple(c.dispositions, disposition),
                        "prior_state_unknown_below"_a = packed(c.prior_state_unknown_below),
                        "acquisition_closed"_a = c.acquisition_closed,
                        "reconcile_attempted"_a = c.reconcile_attempted,
                        "takeover_required"_a = c.takeover_required));
}
ctx::Checkpoint checkpoint(nb::handle h)
{
    ctx::Checkpoint out;
    out.identity = identity(h.attr("identity"));
    out.context = ref(h.attr("context"));
    out.causal_floor = hlc(h.attr("causal_floor"));
    out.processed_after = optional<sdk::Position>(h.attr("processed_after"), position);
    out.writer = optional<ctx::WriterStamp>(h.attr("writer"), stamp);
    out.acquisition = optional<ctx::AcquisitionProvenance>(h.attr("acquisition"), provenance);
    out.last_own_receipt_hlc = optional<Hlc>(h.attr("last_own_receipt_hlc"), hlc);
    out.recovery = optional<ctx::ReconcileCheckpoint>(
            h.attr("recovery"),
            [](nb::handle r)
            {
                ctx::ReconcileCheckpoint item;
                item.transition_id = text(r.attr("transition_id"));
                item.lower_bound = hlc(r.attr("lower_bound"));
                item.recovered_incarnations = list<ctx::RecoveryIncarnation>(
                        r.attr("recovered_incarnations"),
                        [](nb::handle i)
                        {
                            return ctx::RecoveryIncarnation{stamp(i.attr("writer")),
                                                            optional<std::string>(i.attr("marker_operation_id"), text)};
                        });
                item.current_marker_operation_id = optional<std::string>(r.attr("current_marker_operation_id"), text);
                item.marker_hlc = optional<Hlc>(r.attr("marker_hlc"), hlc);
                return item;
            });
    out.unresolved_operations = list<std::string>(h.attr("unresolved_operations"), text);
    out.permanently_unknown_operations =
            list<ctx::UnknownOperation>(h.attr("permanently_unknown_operations"),
                                        [](nb::handle u)
                                        {
                                            ctx::UnknownOperation item;
                                            item.operation_id = text(u.attr("operation_id"));
                                            item.incarnations = list<ctx::WriterStamp>(u.attr("incarnations"), stamp);
                                            item.window = optional<sdk::HlcRange>(u.attr("window"), hlcRange);
                                            item.absence_provable = nb::cast<bool>(u.attr("absence_provable"));
                                            item.normalized_digest =
                                                    optional<std::string>(u.attr("normalized_digest"), binary);
                                            return item;
                                        });
    out.dispositions = list<ctx::OperationDisposition>(
            h.attr("dispositions"),
            [](nb::handle d)
            {
                return ctx::OperationDisposition{reconciled(d.attr("result")),
                                                 stamp(d.attr("prior_writer")),
                                                 optional<std::string>(d.attr("normalized_digest"), binary)};
            });
    out.prior_state_unknown_below = optional<Hlc>(h.attr("prior_state_unknown_below"), hlc);
    out.acquisition_closed = nb::cast<bool>(h.attr("acquisition_closed"));
    out.reconcile_attempted = nb::cast<bool>(h.attr("reconcile_attempted"));
    out.takeover_required = nb::cast<bool>(h.attr("takeover_required"));
    return out;
}
ctx::PageLimits limits(nb::handle options)
{
    ctx::PageLimits out;
    auto input = options.attr("limits");
    if(input.is_none())
        return out;
    if(auto events = input.attr("max_events"); !events.is_none())
        out.max_events = size(events);
    if(auto bytes = input.attr("max_raw_bytes"); !bytes.is_none())
        out.max_raw_bytes = size(bytes);
    return out;
}
void readCalls(nb::handle options, size_t& out)
{
    if(auto calls = options.attr("max_read_calls"); !calls.is_none())
        out = size(calls);
}
ctx::ContextOptions contextOptions(nb::handle h)
{
    ctx::ContextOptions out;
    out.sdk = clientOptions(h);
    auto set = [&](const char* name, size_t& target)
    {
        if(auto v = h.attr(name); !v.is_none())
            target = size(v);
    };
    set("max_writable_sessions", out.max_writable_sessions);
    set("max_pending_operation_bytes", out.max_pending_operation_bytes);
    set("max_completed_operations", out.max_completed_operations);
    set("max_unresolved_operations", out.max_unresolved_operations);
    set("max_persisted_dispositions", out.max_persisted_dispositions);
    set("max_checkpoint_payload_bytes", out.max_checkpoint_payload_bytes);
    if(auto width = h.attr("cut_probe_width_ns"); !width.is_none())
        out.cut_probe_width = std::chrono::nanoseconds(i64(width));
    return out;
}
nb::object pack(const ctx::MemoryResult& r)
{
    return value("MemoryResult",
                 fields("current"_a = pack(r.current),
                        "resolved_prior"_a = tuple(r.resolved_prior, [](const auto& p) { return pack(p); }),
                        "blocking_operation_id"_a = r.blocking_operation_id,
                        "state"_a = enumValue(r.state)));
}
nb::object pack(const ctx::LatestResult& r)
{
    return value("LatestResult",
                 fields("page"_a = pack(r.page),
                        "as_of"_a = packed(r.as_of),
                        "selection_complete"_a = r.selection_complete));
}
nb::object pack(const ctx::ReconcileResult& r)
{
    return value("ReconcileResult",
                 fields("writer"_a = packed(r.writer),
                        "marker_hlc"_a = packed(r.marker_hlc),
                        "range"_a = packed(r.range),
                        "operations"_a = tuple(r.operations, [](const auto& o) { return pack(o); }),
                        "completion"_a = packed(r.completion),
                        "status"_a = status(r.status),
                        "attempted"_a = r.attempted,
                        "permanently_unknown_operations"_a = strings(r.permanently_unknown_operations),
                        "proof_complete"_a = r.proof_complete,
                        "supply_all_unseen_operation_ids"_a = r.supply_all_unseen_operation_ids,
                        "omitted_operation_ids_may_duplicate"_a = r.omitted_operation_ids_may_duplicate));
}
nb::object pack(const ctx::FollowResult& r)
{
    return value("FollowResult",
                 fields("pages"_a = tuple(r.pages,
                                          [](const ctx::FollowPage& p)
                                          {
                                              return value("FollowPage",
                                                           fields("context"_a = pack(p.context),
                                                                  "page"_a = pack(p.page),
                                                                  "resume"_a = packed(p.resume),
                                                                  "starting_cut"_a = packed(p.starting_cut),
                                                                  "uncertified_route_keepers"_a =
                                                                          strings(p.uncertified_route_keepers)));
                                          }),
                        "status"_a = status(r.status),
                        "idle"_a = r.idle));
}
nb::object pack(const ctx::SessionStatus& s)
{
    return value("SessionStatus",
                 fields("context"_a = pack(s.context),
                        "identity"_a = pack(s.identity),
                        "writer"_a = packed(s.writer),
                        "state"_a = enumValue(s.state),
                        "blocking_operation_id"_a = s.blocking_operation_id,
                        "unresolved_operations"_a = strings(s.unresolved_operations),
                        "permanently_unknown_operations"_a = strings(s.permanently_unknown_operations),
                        "reconcile_attempted"_a = s.reconcile_attempted,
                        "causal_floor"_a = pack(s.causal_floor)));
}
} // namespace

void initContext(nb::module_& m)
{
    nb::class_<Session>(m, "ContextSession")
            .def("context", [](Session& s) { return pack(s->context()); })
            .def("identity", [](Session& s) { return pack(s->identity()); })
            .def(
                    "remember",
                    [](Session& s, nb::handle input, nb::handle options, std::optional<double> t)
                    {
                        ctx::Memory memory;
                        memory.operation_id = text(input.attr("operation_id"));
                        memory.envelope = envelope(input.attr("envelope"));
                        memory.durability = static_cast<Durability>(nb::cast<int>(input.attr("durability")));
                        memory.physical = optional<TimeReading>(input.attr("physical"), timeReading);
                        ctx::RememberOptions o{nb::cast<bool>(options.attr("resend_after_absent"))};
                        auto d = deadline(t);
                        auto native = s.shared();
                        return pack(unwrap(call([&] { return native->remember(memory, o, d); })));
                    },
                    "memory"_a,
                    "options"_a,
                    "timeout"_a = nb::none())
            .def(
                    "recall",
                    [](Session& s, nb::handle options, std::optional<double> t)
                    {
                        ctx::RecallOptions o;
                        o.start = optional<Hlc>(options.attr("start"), hlc);
                        o.end = optional<Hlc>(options.attr("end"), hlc);
                        o.cursor = optional<std::string>(options.attr("cursor"), text);
                        o.limits = limits(options);
                        readCalls(options, o.max_read_calls);
                        auto d = deadline(t);
                        auto native = s.shared();
                        return pack(unwrap(call([&] { return native->recall(o, d); })));
                    },
                    "options"_a,
                    "timeout"_a = nb::none())
            .def(
                    "latest",
                    [](Session& s, nb::handle n, nb::handle options, std::optional<double> t)
                    {
                        auto count = size(n);
                        ctx::LatestOptions o;
                        o.before = optional<Hlc>(options.attr("before"), hlc);
                        o.limits = limits(options);
                        readCalls(options, o.max_read_calls);
                        auto d = deadline(t);
                        auto native = s.shared();
                        return pack(unwrap(call([&] { return native->latest(count, o, d); })));
                    },
                    "n"_a,
                    "options"_a,
                    "timeout"_a = nb::none())
            .def(
                    "latest_aggregate",
                    [](Session& s, const std::string& content_type, nb::handle options, std::optional<double> t)
                    {
                        ctx::LatestOptions o;
                        o.before = optional<Hlc>(options.attr("before"), hlc);
                        o.limits = limits(options);
                        readCalls(options, o.max_read_calls);
                        auto d = deadline(t);
                        auto native = s.shared();
                        auto is_aggregate = [content_type](const Event& e)
                        { return e.envelope.content_type == content_type; };
                        return pack(unwrap(call([&] { return native->latestAggregate(is_aggregate, o, d); })));
                    },
                    "content_type"_a,
                    "options"_a,
                    "timeout"_a = nb::none())
            .def(
                    "reconcile",
                    [](Session& s, nb::handle options, std::optional<double> t)
                    {
                        ctx::ReconcileOptions o;
                        o.operation_ids = list<std::string>(options.attr("operation_ids"), text);
                        o.takeover = nb::cast<bool>(options.attr("takeover"));
                        readCalls(options, o.max_read_calls);
                        auto d = deadline(t);
                        auto native = s.shared();
                        return pack(unwrap(call([&] { return native->reconcile(o, d); })));
                    },
                    "options"_a,
                    "timeout"_a = nb::none())
            .def("acknowledge_processed",
                 [](Session& s, nb::handle p)
                 {
                     auto at = position(p);
                     auto native = s.shared();
                     check(call([&] { return native->acknowledgeProcessed(at); }));
                 })
            .def("checkpoint",
                 [](Session& s)
                 {
                     auto native = s.shared();
                     return pack(call([&] { return native->checkpoint(); }));
                 })
            .def("status",
                 [](Session& s)
                 {
                     auto native = s.shared();
                     return pack(call([&] { return native->status(); }));
                 })
            .def(
                    "close",
                    [](Session& s, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = s.shared();
                        auto result = unwrap(call([&] { return native->close(d); }));
                        return value(
                                "CloseResult",
                                fields("release_committed"_a = result.release_committed, "fenced"_a = result.fenced));
                    },
                    "timeout"_a = nb::none());
    nb::class_<ContextClient>(m, "ContextClient")
            .def(
                    "ensure_context",
                    [](ContextClient& c, const std::string& chronicle, const std::string& name, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->ensureContext(chronicle, name, d); })));
                    },
                    "chronicle"_a,
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "list_contexts",
                    [](ContextClient& c, const std::string& chronicle, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto refs = unwrap(call([&] { return native->listContexts(chronicle, d); }));
                        return tuple(refs, [](const ctx::ContextRef& r) { return pack(r); });
                    },
                    "chronicle"_a,
                    "timeout"_a = nb::none())
            .def(
                    "open",
                    [](ContextClient& c,
                       nb::handle context,
                       nb::handle agent,
                       nb::handle options,
                       std::optional<double> t)
                    {
                        auto r = ref(context);
                        auto i = identity(agent);
                        ctx::OpenOptions o;
                        o.access = static_cast<ctx::Access>(nb::cast<int>(options.attr("access")));
                        o.session_id = text(options.attr("session_id"));
                        o.resume = optional<ctx::Checkpoint>(options.attr("resume"), checkpoint);
                        o.ownership = optional<ctx::AcquisitionProvenance>(options.attr("ownership"), provenance);
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto session = unwrap(call([&] { return native->open(std::move(r), i, std::move(o), d); }));
                        return new Session(std::move(session));
                    },
                    "context"_a,
                    "identity"_a,
                    "options"_a,
                    "timeout"_a = nb::none())
            .def(
                    "follow",
                    [](ContextClient& c, nb::handle inputs, nb::handle options, std::optional<double> t)
                    {
                        auto native_inputs = list<ctx::FollowInput>(
                                inputs,
                                [](nb::handle h)
                                {
                                    ctx::FollowInput item;
                                    item.session = nb::cast<Session&>(h.attr("session").attr("_handle")).shared();
                                    item.from = static_cast<ctx::FollowFrom>(nb::cast<int>(h.attr("from_")));
                                    item.after = optional<sdk::Position>(h.attr("after"), position);
                                    return item;
                                });
                        ctx::FollowOptions o;
                        o.limits = limits(options);
                        if(auto wait = options.attr("wait"); !wait.is_none())
                        {
                            // The native wait has whole-second granularity; a partial second rounds up.
                            auto seconds = nb::cast<double>(wait);
                            if(!std::isfinite(seconds) || seconds < 0 || seconds > 60)
                                throw nb::value_error("wait must be between 0 and 60 seconds");
                            o.wait = std::chrono::seconds(static_cast<int64_t>(std::ceil(seconds)));
                        }
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->follow(native_inputs, o, d); })));
                    },
                    "inputs"_a,
                    "options"_a,
                    "timeout"_a = nb::none());
    m.def(
            "connect_context",
            [](nb::handle options, std::optional<double> t)
            {
                auto config = contextOptions(options);
                auto d = deadline(t ? *t : nb::cast<double>(options.attr("timeout")));
                auto client = unwrap(call([&] { return ctx::ContextClient::Connect(std::move(config), d); }));
                return new ContextClient(std::make_shared<ctx::ContextClient>(std::move(client)));
            },
            "options"_a,
            "timeout"_a = nb::none());
    m.def(
            "encode_checkpoint",
            [](nb::handle value, nb::handle max_bytes)
            {
                auto input = checkpoint(value);
                auto encoded = max_bytes.is_none() ? ctx::encodeCheckpoint(input)
                                                   : ctx::encodeCheckpoint(input, size(max_bytes));
                return data(unwrap(std::move(encoded)));
            },
            "checkpoint"_a,
            "max_bytes"_a = nb::none());
    m.def("decode_checkpoint", [](nb::handle encoded) { return pack(unwrap(ctx::decodeCheckpoint(binary(encoded)))); });
}
} // namespace binding
