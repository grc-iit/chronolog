#include "binding.h"
#include <climits>

namespace binding
{
namespace
{
nb::object module() { return nb::module_::import_("chronolog"); }
nb::object refusal(const AcquireRefusal& r)
{
    return value("AcquireRefusal",
                 fields("refusal_reason"_a = static_cast<int>(r.refusal_reason),
                        "current_incarnation"_a = r.current_incarnation,
                        "matched_incarnation"_a = r.matched_incarnation,
                        "remaining_ns"_a = r.remaining_ns,
                        "termination_cause"_a = maybe(r.termination_cause,
                                                      [](auto cause) { return nb::int_(static_cast<int>(cause)); })));
}
nb::object refusalOf(const absl::Status& value)
{
    auto found = sdk::acquireRefusalOf(value);
    return found ? refusal(*found) : nb::none();
}
} // namespace

nb::object status(const absl::Status& value)
{
    return module().attr("_status")(static_cast<int>(value.code()),
                                    std::string(value.message()),
                                    static_cast<int>(sdk::rejectionOf(value)),
                                    refusalOf(value));
}
nb::object error(const absl::Status& value)
{
    return module().attr("_error")(static_cast<int>(value.code()),
                                   std::string(value.message()),
                                   static_cast<int>(sdk::rejectionOf(value)),
                                   refusalOf(value));
}
void raise(const absl::Status& value)
{
    auto e = error(value);
    PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(e.ptr())), e.ptr());
    throw nb::python_error();
}
sdk::Deadline deadline(std::optional<double> timeout)
{
    if(!timeout)
        return {};
    if(!std::isfinite(*timeout) || *timeout <= 0 || *timeout > 315360000)
        throw nb::value_error("timeout must be finite, positive, and at most ten years");
    return std::chrono::system_clock::now() +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(*timeout));
}
namespace
{
void integer(nb::handle input)
{
    if(!PyLong_Check(input.ptr()) || PyBool_Check(input.ptr()))
        throw nb::type_error("ids, counts and nanoseconds must be int");
}
[[noreturn]] void overflow(const char* message)
{
    PyErr_SetString(PyExc_OverflowError, message);
    throw nb::python_error();
}
} // namespace
uint64_t u64(nb::handle input)
{
    integer(input);
    auto result = PyLong_AsUnsignedLongLong(input.ptr());
    if(result == static_cast<unsigned long long>(-1) && PyErr_Occurred())
    {
        PyErr_Clear();
        overflow("int outside uint64 range");
    }
    return result;
}
int64_t i64(nb::handle input)
{
    integer(input);
    auto result = PyLong_AsLongLong(input.ptr());
    if(result == -1 && PyErr_Occurred())
    {
        PyErr_Clear();
        overflow("int outside int64 range");
    }
    return result;
}
uint32_t u32(nb::handle input)
{
    auto result = u64(input);
    if(result > UINT32_MAX)
        overflow("int outside uint32 range");
    return static_cast<uint32_t>(result);
}
std::string text(nb::handle input)
{
    if(!nb::isinstance<nb::str>(input))
        throw nb::type_error("expected str");
    return nb::cast<std::string>(input);
}
std::string binary(nb::handle input)
{
    if(!nb::isinstance<nb::bytes>(input))
        throw nb::type_error("payloads, digests and trace fields must be bytes");
    auto b = nb::borrow<nb::bytes>(input);
    return {b.c_str(), b.size()};
}

nb::object pack(const Hlc& h) { return value("Hlc", fields("physical_ns"_a = h.physical_ns, "logical"_a = h.logical)); }
nb::object pack(const EventId& id)
{
    return value("EventId",
                 fields("story_id"_a = id.story_id,
                        "writer_id"_a = id.writer_id,
                        "incarnation"_a = id.incarnation,
                        "sequence"_a = id.sequence));
}
nb::object pack(const sdk::Position& p)
{
    return value("Position", fields("hlc"_a = pack(p.hlc), "id"_a = pack(p.id)));
}
nb::object pack(const sdk::HlcRange& r)
{
    return value("HlcRange", fields("start"_a = pack(r.start), "end"_a = pack(r.end)));
}
namespace
{
nb::object pack(const KeeperRef& k)
{
    return value("KeeperRef", fields("process_id"_a = k.process_id, "endpoint"_a = k.endpoint));
}
nb::object pack(const Route& r)
{
    return value("Route",
                 fields("epoch"_a = r.epoch,
                        "keepers"_a = tuple(r.keepers, [](const KeeperRef& k) { return pack(k); }),
                        "grapher"_a = r.grapher,
                        "player"_a = r.player));
}
nb::object pack(const Chronicle& c)
{
    return value("Chronicle", fields("name"_a = c.name, "tombstoned"_a = c.tombstoned));
}
nb::object pack(const Story& s)
{
    return value("Story",
                 fields("id"_a = s.id,
                        "chronicle"_a = s.chronicle,
                        "name"_a = s.name,
                        "epoch"_a = s.epoch,
                        "tombstoned"_a = s.tombstoned));
}
nb::object pack(const AcquisitionLease& lease)
{
    return value("AcquisitionLease",
                 fields("duration_ns"_a = lease.duration_ns, "remaining_ns"_a = lease.remaining_ns));
}
nb::object pack(const Acquisition& a)
{
    return value("Acquisition",
                 fields("story_id"_a = a.story_id,
                        "writer_id"_a = a.writer_id,
                        "incarnation"_a = a.incarnation,
                        "route"_a = pack(a.route),
                        "assigned_keeper"_a = pack(a.assigned_keeper),
                        "lease"_a = pack(a.lease),
                        "keeper_preference"_a =
                                maybe(a.keeper_preference, [](auto p) { return nb::int_(static_cast<int>(p)); })));
}
nb::object pack(const sdk::WriterLease& lease)
{
    return value("WriterLease",
                 fields("grant"_a = pack(lease.grant),
                        "estimated_remaining_ns"_a = lease.estimated_remaining_ns,
                        "confirmed"_a = lease.confirmed,
                        "termination_cause"_a =
                                maybe(lease.termination_cause, [](auto c) { return nb::int_(static_cast<int>(c)); }),
                        "renewals"_a = lease.renewals));
}
nb::object pack(const Link& l)
{
    return value("Link",
                 fields("type"_a = l.type,
                        "target"_a = binding::pack(l.target),
                        "target_hlc"_a = maybe(l.target_hlc, [](const Hlc& h) { return binding::pack(h); })));
}
nb::object pack(const Envelope& e)
{
    return value("Envelope",
                 fields("content_type"_a = e.content_type,
                        "payload"_a = nb::bytes(e.payload.data(), e.payload.size()),
                        "trace_id"_a = nb::bytes(e.trace_id.data(), e.trace_id.size()),
                        "span_id"_a = nb::bytes(e.span_id.data(), e.span_id.size()),
                        "attributes"_a = e.attributes,
                        "kind"_a = e.kind,
                        "actor"_a = e.actor,
                        "links"_a = tuple(e.links, [](const Link& l) { return pack(l); })));
}
} // namespace
nb::object pack(const sdk::AppendResult& r)
{
    return value("AppendResult",
                 fields("event_id"_a = pack(r.event_id),
                        "hlc"_a = pack(r.hlc),
                        "durability"_a = static_cast<int>(r.achieved)));
}
nb::object pack(const Event& e)
{
    auto physical = value("TimeReading",
                          fields("physical_ns"_a = e.physical.physical_ns,
                                 "uncertainty_ns"_a = e.physical.uncertainty_ns,
                                 "status"_a = static_cast<int>(e.physical.status)));
    return value("Event",
                 fields("id"_a = pack(e.id),
                        "hlc"_a = pack(e.hlc),
                        "physical"_a = physical,
                        "envelope"_a = pack(e.envelope),
                        "durability"_a = static_cast<int>(e.durability)));
}
nb::object pack(const sdk::AwaitResult& r)
{
    return value("AwaitResult",
                 fields("answer"_a = static_cast<int>(r.answer),
                        "event"_a = maybe(r.event, [](const Event& e) { return pack(e); }),
                        "frontier"_a = maybe(r.frontier, [](const Hlc& h) { return pack(h); })));
}
nb::object pack(const Completion& c)
{
    return value("Completion",
                 fields("complete"_a = c.complete,
                        "frontier"_a = pack(c.frontier),
                        "laggards"_a = tuple(c.laggards,
                                             [](const Frontier& f)
                                             {
                                                 return value("Frontier",
                                                              fields("writer_id"_a = f.writer_id,
                                                                     "incarnation"_a = f.incarnation,
                                                                     "frontier"_a = pack(f.frontier)));
                                             }),
                        "reason"_a = static_cast<int>(c.reason),
                        "claim_start"_a = maybe(c.claim_start, [](const Hlc& h) { return pack(h); }),
                        "claim_end"_a = maybe(c.claim_end, [](const Hlc& h) { return pack(h); })));
}
Hlc hlc(nb::handle h) { return {i64(h.attr("physical_ns")), u32(h.attr("logical"))}; }
EventId eventId(nb::handle h)
{
    return {u64(h.attr("story_id")), u64(h.attr("writer_id")), u64(h.attr("incarnation")), u64(h.attr("sequence"))};
}
sdk::Position position(nb::handle h) { return {hlc(h.attr("hlc")), eventId(h.attr("id"))}; }
sdk::HlcRange hlcRange(nb::handle h) { return {hlc(h.attr("start")), hlc(h.attr("end"))}; }
EventPredicate predicate(nb::handle h)
{
    EventPredicate out;
    if(h.is_none())
        return out;
    out.kinds = list<std::string>(h.attr("kinds"), text);
    out.actors = list<std::string>(h.attr("actors"), text);
    out.attributes = list<EventPredicate::Attribute>(h.attr("attributes"),
                                                     [](nb::handle t)
                                                     { return EventPredicate::Attribute{text(t[0]), text(t[1])}; });
    out.links = list<EventPredicate::LinkTerm>(h.attr("links"),
                                               [](nb::handle t)
                                               { return EventPredicate::LinkTerm{text(t[0]), eventId(t[1])}; });
    out.event_ids = list<EventId>(h.attr("event_ids"), eventId);
    return out;
}
TimeReading timeReading(nb::handle h)
{
    return {i64(h.attr("physical_ns")),
            optional<uint64_t>(h.attr("uncertainty_ns"), u64),
            static_cast<ClockStatus>(nb::cast<int>(h.attr("status")))};
}
Link eventLink(nb::handle h)
{
    return {text(h.attr("type")), eventId(h.attr("target")), optional<Hlc>(h.attr("target_hlc"), hlc)};
}
Envelope envelope(nb::handle e)
{
    return {text(e.attr("content_type")),
            binary(e.attr("payload")),
            binary(e.attr("trace_id")),
            binary(e.attr("span_id")),
            nb::cast<std::map<std::string, std::string>>(e.attr("attributes")),
            text(e.attr("kind")),
            text(e.attr("actor")),
            list<Link>(e.attr("links"), eventLink)};
}
sdk::ClientOptions clientOptions(nb::handle h)
{
    sdk::ClientOptions o;
    o.catalog_endpoint = text(h.attr("catalog"));
    auto player = h.attr("player");
    o.player_endpoint = player.is_none() ? std::string() : text(player);
    auto timeout = nb::cast<double>(h.attr("timeout"));
    (void)deadline(timeout);
    o.rpc_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeout));
    o.retry.max_retries = u64(h.attr("max_retries"));
    auto backoff = nb::cast<double>(h.attr("retry_backoff"));
    if(!std::isfinite(backoff) || backoff < 0 || backoff > 3600)
        throw nb::value_error("invalid retry_backoff");
    o.retry.backoff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(backoff));
    o.max_in_flight = u64(h.attr("max_in_flight"));
    o.batch_size = u64(h.attr("batch_size"));
    o.max_batch_items = u64(h.attr("max_batch_items"));
    o.max_batch_bytes = u64(h.attr("max_batch_bytes"));
    auto args = h.attr("channel_args");
    if(!args.is_none())
        for(auto [k, v]: nb::cast<nb::dict>(args))
        {
            auto key = text(k);
            if(nb::isinstance<nb::str>(v))
                o.channel_args[key] = nb::cast<std::string>(v);
            else
            {
                auto number = i64(v);
                if(number < INT_MIN || number > INT_MAX)
                    overflow("channel argument outside int32 range");
                o.channel_args[key] = static_cast<int>(number);
            }
        }
    return o;
}

Runtime::Runtime()
    : pid_(getpid())
    , reaper_([this] { reap(); })
{}
Runtime& Runtime::get()
{
    // Callers hold the GIL. The parent's Runtime is leaked in a forked child, never locked or joined there.
    static Runtime* current = nullptr;
    if(!current || current->pid_ != getpid())
        current = new Runtime();
    return *current;
}
void Runtime::enter()
{
    {
        std::lock_guard lock(mutex_);
        if(!closing_)
        {
            ++inflight_;
            return;
        }
    }
    raise(absl::CancelledError("interpreter is shutting down"));
}
void Runtime::leave()
{
    std::lock_guard lock(mutex_);
    --inflight_;
    changed_.notify_all();
}
void Runtime::reap()
{
    std::unique_lock lock(mutex_);
    for(;;)
    {
        changed_.wait(lock, [&] { return closed_ || !graveyard_.empty(); });
        while(!graveyard_.empty())
        {
            auto item = std::move(graveyard_.front());
            graveyard_.pop_front();
            lock.unlock();
            item.reset();
            lock.lock();
        }
        if(closed_)
            return;
    }
}
void Runtime::bury(std::shared_ptr<void> item)
{
    {
        std::lock_guard lock(mutex_);
        if(!closed_)
        {
            graveyard_.push_back(std::move(item));
            changed_.notify_all();
            return;
        }
    }
    // After shutdown no call is in flight and no thread waits on the GIL, so the object drops here.
    nb::gil_scoped_release release;
    item.reset();
}
void Runtime::track(std::weak_ptr<sdk::ReadStream> stream)
{
    std::lock_guard lock(mutex_);
    std::erase_if(reads_, [](const auto& item) { return item.expired(); });
    reads_.push_back(std::move(stream));
}
void Runtime::track(std::weak_ptr<sdk::TailStream> stream)
{
    std::lock_guard lock(mutex_);
    std::erase_if(tails_, [](const auto& item) { return item.expired(); });
    tails_.push_back(std::move(stream));
}
void Runtime::shutdown()
{
    std::vector<std::shared_ptr<sdk::ReadStream>> reads;
    std::vector<std::shared_ptr<sdk::TailStream>> tails;
    {
        std::lock_guard lock(mutex_);
        if(closing_)
            return;
        closing_ = true;
        for(const auto& item: reads_)
            if(auto stream = item.lock())
                reads.push_back(std::move(stream));
        for(const auto& item: tails_)
            if(auto stream = item.lock())
                tails.push_back(std::move(stream));
    }
    for(const auto& stream: reads) stream->cancel();
    for(const auto& stream: tails) stream->cancel();
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return inflight_ == 0; });
        closed_ = true;
        changed_.notify_all();
    }
    reaper_.join();
    // The last references may be the only ones left; they drop on this thread.
    reads.clear();
    tails.clear();
}

namespace
{
using binding::pack;
using Client = Held<sdk::Client>;
using Writer = Held<sdk::Writer>;
using LaneWriter = Held<sdk::LaneWriter>;
using ReadStream = Held<sdk::ReadStream>;
using TailStream = Held<sdk::TailStream>;

sdk::AppendSpec spec(nb::handle h)
{
    auto stamp = h.attr("physical");
    return {envelope(h.attr("envelope")),
            static_cast<Durability>(nb::cast<int>(h.attr("durability"))),
            optional<TimeReading>(stamp, timeReading)};
}
AcquireOptions acquireOptions(nb::handle h)
{
    AcquireOptions o;
    if(h.is_none())
        return o;
    o.lease_duration_ns = optional<int64_t>(h.attr("lease_duration_ns"), i64);
    o.preferred_keeper_process_id = optional<std::string>(h.attr("preferred_keeper_process_id"), text);
    o.takeover = nb::cast<bool>(h.attr("takeover"));
    o.expected_prior_incarnation = optional<uint64_t>(h.attr("expected_prior_incarnation"), u64);
    auto id = h.attr("acquire_request_id");
    if(!id.is_none())
        o.acquire_request_id = text(id);
    return o;
}
template <class W>
void appends(nb::class_<Held<W>>& cls)
{
    cls.def(
               "append",
               [](Held<W>& w, nb::handle item, std::optional<double> timeout)
               {
                   auto s = spec(item);
                   auto d = deadline(timeout);
                   auto native = w.shared();
                   return pack(unwrap(call([&] { return native->append(s, d); })));
               },
               "item"_a,
               "timeout"_a = nb::none())
            .def(
                    "append_batch",
                    [](Held<W>& w, nb::list items, std::optional<double> timeout)
                    {
                        std::vector<sdk::AppendSpec> specs;
                        for(auto h: items) specs.push_back(spec(h));
                        auto d = deadline(timeout);
                        auto native = w.shared();
                        auto results = unwrap(call([&] { return native->appendBatch(specs, d); }));
                        nb::list out;
                        for(auto& r: results) out.append(r.ok() ? pack(*r) : error(r.status()));
                        return out;
                    },
                    "items"_a,
                    "timeout"_a = nb::none())
            .def(
                    "release",
                    [](Held<W>& w, std::optional<double> timeout)
                    {
                        auto d = deadline(timeout);
                        auto native = w.shared();
                        return unwrap(call([&] { return native->release(d); }));
                    },
                    "timeout"_a = nb::none());
}
template <class S>
void stream(nb::module_& m, const char* name)
{
    nb::class_<Held<S>>(m, name)
            .def("cancel",
                 [](Held<S>& s)
                 {
                     auto native = s.shared();
                     nb::gil_scoped_release release;
                     native->cancel();
                 })
            .def(
                    "next",
                    [](Held<S>& s, std::optional<double> timeout) -> nb::object
                    {
                        auto d = deadline(timeout);
                        auto native = s.shared();
                        auto item = unwrap(call([&] { return native->next(d); }));
                        if(!item)
                            return nb::none();
                        nb::list events;
                        for(auto& e: item->events) events.append(pack(e));
                        return nb::make_tuple(events,
                                              item->completion ? pack(*item->completion) : nb::none(),
                                              item->continuation ? pack(*item->continuation) : nb::none());
                    },
                    "timeout"_a = nb::none());
}
template <class S>
Held<S>* held(absl::StatusOr<S> result)
{
    auto native = std::make_shared<S>(std::move(unwrap(std::move(result))));
    Runtime::get().track(std::weak_ptr<S>(native));
    return new Held<S>(std::move(native));
}
} // namespace
} // namespace binding

using namespace binding;
NB_MODULE(_core, m)
{
    stream<sdk::ReadStream>(m, "ReadStream");
    stream<sdk::TailStream>(m, "TailStream");
    nb::class_<Writer> writer(m, "Writer");
    writer.def("acquisition", [](Writer& w) { return pack(w->acquisition()); })
            .def("lease", [](Writer& w) { return pack(w->lease()); });
    appends(writer);
    nb::class_<LaneWriter> lanes(m, "LaneWriter");
    lanes.def("lanes", [](LaneWriter& w) { return w->lanes(); });
    appends(lanes);
    nb::class_<Client>(m, "Client")
            .def(
                    "create_chronicle",
                    [](Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->createChronicle(n, d); })));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "chronicle",
                    [](Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->getChronicle(n, d); })));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "list_chronicles",
                    [](Client& c, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto r = unwrap(call([&] { return native->listChronicles(d); }));
                        nb::list out;
                        for(auto& x: r) out.append(pack(x));
                        return out;
                    },
                    "timeout"_a = nb::none())
            .def(
                    "destroy_chronicle",
                    [](Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        check(call([&] { return native->destroyChronicle(n, d); }));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "create_story",
                    [](Client& c, const std::string& n, const std::string& s, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->createStory(n, s, d); })));
                    },
                    "chronicle"_a,
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "story",
                    [](Client& c, nb::handle id, std::optional<double> t)
                    {
                        auto story = u64(id);
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->getStory(story, d); })));
                    },
                    "id"_a,
                    "timeout"_a = nb::none())
            .def(
                    "list_stories",
                    [](Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto r = unwrap(call([&] { return native->listStories(n, d); }));
                        nb::list out;
                        for(auto& x: r) out.append(pack(x));
                        return out;
                    },
                    "chronicle"_a,
                    "timeout"_a = nb::none())
            .def(
                    "destroy_story",
                    [](Client& c, nb::handle id, std::optional<double> t)
                    {
                        auto story = u64(id);
                        auto d = deadline(t);
                        auto native = c.shared();
                        check(call([&] { return native->destroyStory(story, d); }));
                    },
                    "id"_a,
                    "timeout"_a = nb::none())
            .def(
                    "acquire",
                    [](Client& c,
                       nb::handle id,
                       const std::string& identity,
                       nb::handle options,
                       std::optional<double> t)
                    {
                        auto story = u64(id);
                        auto o = acquireOptions(options);
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto writer = unwrap(call([&] { return native->acquire(story, identity, std::move(o), d); }));
                        return new Writer(std::make_shared<sdk::Writer>(std::move(writer)));
                    },
                    "id"_a,
                    "identity"_a,
                    "options"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def(
                    "acquire_lanes",
                    [](Client& c,
                       nb::handle id,
                       const std::string& identity,
                       nb::handle lanes,
                       nb::handle slice_ns,
                       nb::handle options,
                       std::optional<double> t)
                    {
                        auto story = u64(id);
                        auto count = u64(lanes);
                        auto slice = i64(slice_ns);
                        auto o = acquireOptions(options);
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto writer = unwrap(call(
                                [&] { return native->acquireLanes(story, identity, count, slice, std::move(o), d); }));
                        return new LaneWriter(std::make_shared<sdk::LaneWriter>(std::move(writer)));
                    },
                    "id"_a,
                    "identity"_a,
                    "lanes"_a,
                    "slice_ns"_a,
                    "options"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def("new_acquire_request_id", [](Client& c) { return unwrap(c->newAcquireRequestId()); })
            .def(
                    "await_event",
                    [](Client& c, nb::handle ref, nb::handle at, double bound_s, std::optional<double> t)
                    {
                        if(!std::isfinite(bound_s) || bound_s < 0 || bound_s > 315360000)
                            throw nb::value_error("bound_s must be finite, nonnegative, and at most ten years");
                        sdk::EventRef r{eventId(ref), optional<Hlc>(at, hlc)};
                        auto bound = std::chrono::nanoseconds(static_cast<int64_t>(bound_s * 1e9));
                        auto d = deadline(t);
                        auto native = c.shared();
                        return pack(unwrap(call([&] { return native->await(r, bound, d); })));
                    },
                    "ref"_a,
                    "hlc"_a = nb::none(),
                    "bound_s"_a = 0.0,
                    "timeout"_a = nb::none())
            .def(
                    "read",
                    [](Client& c,
                       nb::handle id,
                       nb::handle start,
                       nb::handle end,
                       nb::handle where,
                       bool newest_first,
                       std::optional<uint32_t> max_events,
                       std::optional<double> t)
                    {
                        auto story = u64(id);
                        sdk::HlcRange r{hlc(start), hlc(end)};
                        sdk::ReadOptions options;
                        options.predicate = predicate(where);
                        options.max_events = max_events;
                        if(newest_first)
                            options.order = sdk::ReadOrder::NewestFirst;
                        auto d = deadline(t);
                        auto native = c.shared();
                        return held(call([&] { return native->read(story, r, options, d); }));
                    },
                    "id"_a,
                    "start"_a,
                    "end"_a,
                    "predicate"_a = nb::none(),
                    "newest_first"_a = false,
                    "max_events"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def(
                    "read_prefix",
                    [](Client& c,
                       const std::string& prefix,
                       nb::handle start,
                       nb::handle end,
                       nb::handle where,
                       bool newest_first,
                       std::optional<uint32_t> max_events,
                       std::optional<double> t)
                    {
                        sdk::HlcRange r{hlc(start), hlc(end)};
                        sdk::ReadOptions options;
                        options.predicate = predicate(where);
                        options.max_events = max_events;
                        if(newest_first)
                            options.order = sdk::ReadOrder::NewestFirst;
                        auto d = deadline(t);
                        auto native = c.shared();
                        return held(call([&] { return native->read(prefix, r, options, d); }));
                    },
                    "prefix"_a,
                    "start"_a,
                    "end"_a,
                    "predicate"_a = nb::none(),
                    "newest_first"_a = false,
                    "max_events"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def(
                    "read_physical",
                    [](Client& c, nb::handle id, nb::handle start, nb::handle end, std::optional<double> t)
                    {
                        auto story = u64(id);
                        sdk::PhysicalRange r{i64(start), i64(end)};
                        auto d = deadline(t);
                        auto native = c.shared();
                        return held(call([&] { return native->readPhysical(story, r, d); }));
                    },
                    "id"_a,
                    "start"_a,
                    "end"_a,
                    "timeout"_a = nb::none())
            .def(
                    "tail",
                    [](Client& c, nb::handle id, nb::handle after, nb::handle where, std::optional<double> t)
                    {
                        auto story = u64(id);
                        auto p = optional<sdk::Position>(after, position);
                        sdk::TailOptions options;
                        options.predicate = predicate(where);
                        auto d = deadline(t);
                        auto native = c.shared();
                        return held(call([&] { return native->tail(story, p, options, d); }));
                    },
                    "id"_a,
                    "after"_a = nb::none(),
                    "predicate"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def(
                    "tail_prefix",
                    [](Client& c,
                       const std::string& prefix,
                       nb::handle after,
                       nb::handle where,
                       std::optional<double> t)
                    {
                        auto p = optional<sdk::Position>(after, position);
                        sdk::TailOptions options;
                        options.predicate = predicate(where);
                        auto d = deadline(t);
                        auto native = c.shared();
                        return held(call([&] { return native->tail(prefix, p, options, d); }));
                    },
                    "prefix"_a,
                    "after"_a = nb::none(),
                    "predicate"_a = nb::none(),
                    "timeout"_a = nb::none())
            .def(
                    "list_stories_by_prefix",
                    [](Client& c, const std::string& prefix, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto native = c.shared();
                        auto r = unwrap(call([&] { return native->listStoriesByPrefix(prefix, d); }));
                        nb::list out;
                        for(auto& x: r) out.append(pack(x));
                        return out;
                    },
                    "prefix"_a,
                    "timeout"_a = nb::none());
    m.def("connect",
          [](nb::handle options)
          {
              auto config = clientOptions(options);
              auto d = deadline(nb::cast<double>(options.attr("timeout")));
              auto client = unwrap(call([&] { return sdk::Client::Connect(std::move(config), d); }));
              return new Client(std::make_shared<sdk::Client>(std::move(client)));
          });
    m.def("_shutdown",
          []
          {
              auto& runtime = Runtime::get();
              nb::gil_scoped_release release;
              runtime.shutdown();
          });
    initContext(m);
}
