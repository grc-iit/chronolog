#include "chronolog/client/client.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <cmath>

namespace nb = nanobind;
using namespace nb::literals;
using namespace chronolog;
namespace sdk = chronolog::client;
namespace
{
nb::object value(const char* name, nb::dict fields) { return nb::module_::import_("chronolog").attr(name)(**fields); }
nb::object error(const absl::Status& status)
{
    return nb::module_::import_("chronolog")
            .attr("_error")(static_cast<int>(status.code()), std::string(status.message()));
}
void check(const absl::Status& status)
{
    if(!status.ok())
    {
        auto e = error(status);
        PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(e.ptr())), e.ptr());
        throw nb::python_error();
    }
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
template <class F>
auto call(F&& f)
{
    return [&]
    {
        nb::gil_scoped_release release;
        return f();
    }();
}
template <class T>
T unwrap(absl::StatusOr<T> result)
{
    check(result.status());
    return std::move(*result);
}
nb::object pack(Hlc h) { return value("Hlc", nb::dict("physical_ns"_a = h.physical_ns, "logical"_a = h.logical)); }
nb::object pack(EventId id)
{
    return value("EventId",
                 nb::dict("story_id"_a = id.story_id,
                          "writer_id"_a = id.writer_id,
                          "incarnation"_a = id.incarnation,
                          "sequence"_a = id.sequence));
}
nb::object pack(KeeperRef k)
{
    return value("KeeperRef", nb::dict("process_id"_a = k.process_id, "endpoint"_a = k.endpoint));
}
nb::object pack(Route r)
{
    nb::list keepers;
    for(auto& k: r.keepers) keepers.append(pack(k));
    return value("Route",
                 nb::dict("epoch"_a = r.epoch,
                          "keepers"_a = nb::tuple(keepers),
                          "grapher"_a = r.grapher,
                          "player"_a = r.player));
}
nb::object pack(Chronicle c) { return value("Chronicle", nb::dict("name"_a = c.name, "tombstoned"_a = c.tombstoned)); }
nb::object pack(Story s)
{
    return value("Story",
                 nb::dict("id"_a = s.id,
                          "chronicle"_a = s.chronicle,
                          "name"_a = s.name,
                          "epoch"_a = s.epoch,
                          "tombstoned"_a = s.tombstoned));
}
nb::object pack(sdk::AppendResult r)
{
    return value("AppendResult",
                 nb::dict("event_id"_a = pack(r.event_id),
                          "hlc"_a = pack(r.hlc),
                          "durability"_a = static_cast<int>(r.achieved)));
}
nb::object pack(Envelope e)
{
    return value("Envelope",
                 nb::dict("content_type"_a = e.content_type,
                          "payload"_a = nb::bytes(e.payload.data(), e.payload.size()),
                          "trace_id"_a = nb::bytes(e.trace_id.data(), e.trace_id.size()),
                          "span_id"_a = nb::bytes(e.span_id.data(), e.span_id.size()),
                          "attributes"_a = e.attributes));
}
nb::object pack(Event e)
{
    auto physical = value("TimeReading",
                          nb::dict("physical_ns"_a = e.physical.physical_ns,
                                   "uncertainty_ns"_a = e.physical.uncertainty_ns,
                                   "status"_a = static_cast<int>(e.physical.status)));
    return value("Event",
                 nb::dict("id"_a = pack(e.id),
                          "hlc"_a = pack(e.hlc),
                          "physical"_a = physical,
                          "envelope"_a = pack(e.envelope),
                          "durability"_a = static_cast<int>(e.durability)));
}
nb::object pack(Completion c)
{
    nb::list laggards;
    for(auto& f: c.laggards)
        laggards.append(value("Frontier",
                              nb::dict("writer_id"_a = f.writer_id,
                                       "incarnation"_a = f.incarnation,
                                       "frontier"_a = pack(f.frontier))));
    return value("Completion",
                 nb::dict("complete"_a = c.complete,
                          "frontier"_a = pack(c.frontier),
                          "laggards"_a = nb::tuple(laggards),
                          "reason"_a = static_cast<int>(c.reason)));
}
Hlc hlc(nb::handle h) { return {nb::cast<int64_t>(h.attr("physical_ns")), nb::cast<uint32_t>(h.attr("logical"))}; }
EventId eventId(nb::handle h)
{
    return {nb::cast<uint64_t>(h.attr("story_id")),
            nb::cast<uint64_t>(h.attr("writer_id")),
            nb::cast<uint64_t>(h.attr("incarnation")),
            nb::cast<uint64_t>(h.attr("sequence"))};
}
std::string binary(nb::handle h)
{
    auto b = nb::cast<nb::bytes>(h);
    return {b.c_str(), b.size()};
}
sdk::AppendSpec spec(nb::handle h)
{
    auto e = h.attr("envelope");
    return {{nb::cast<std::string>(e.attr("content_type")),
             binary(e.attr("payload")),
             binary(e.attr("trace_id")),
             binary(e.attr("span_id")),
             nb::cast<std::map<std::string, std::string>>(e.attr("attributes"))},
            static_cast<Durability>(nb::cast<int>(h.attr("durability")))};
}
template <class S>
void stream(nb::module_& m, const char* name)
{
    nb::class_<S>(m, name)
            .def("cancel", [](S& s) { call([&] { s.cancel(); }); })
            .def(
                    "next",
                    [](S& s, std::optional<double> timeout) -> nb::object
                    {
                        auto d = deadline(timeout);
                        auto item = unwrap(call([&] { return s.next(d); }));
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
} // namespace
NB_MODULE(_core, m)
{
    stream<sdk::ReadStream>(m, "ReadStream");
    stream<sdk::TailStream>(m, "TailStream");
    nb::class_<sdk::Writer>(m, "Writer")
            .def("acquisition",
                 [](sdk::Writer& w)
                 {
                     auto a = call([&] { return w.acquisition(); });
                     return nb::dict("story_id"_a = a.story_id,
                                     "writer_id"_a = a.writer_id,
                                     "incarnation"_a = a.incarnation,
                                     "route"_a = pack(a.route),
                                     "assigned_keeper"_a = pack(a.assigned_keeper));
                 })
            .def(
                    "append",
                    [](sdk::Writer& w, nb::handle item, std::optional<double> timeout)
                    {
                        auto s = spec(item);
                        auto d = deadline(timeout);
                        return pack(unwrap(call([&] { return w.append(s, d); })));
                    },
                    "item"_a,
                    "timeout"_a = nb::none())
            .def(
                    "append_batch",
                    [](sdk::Writer& w, nb::list items, std::optional<double> timeout)
                    {
                        std::vector<sdk::AppendSpec> specs;
                        for(auto h: items) specs.push_back(spec(h));
                        auto d = deadline(timeout);
                        auto results = unwrap(call([&] { return w.appendBatch(specs, d); }));
                        nb::list out;
                        for(auto& r: results) out.append(r.ok() ? pack(*r) : error(r.status()));
                        return out;
                    },
                    "items"_a,
                    "timeout"_a = nb::none())
            .def(
                    "release",
                    [](sdk::Writer& w, std::optional<double> timeout)
                    {
                        auto d = deadline(timeout);
                        return unwrap(call([&] { return w.release(d); }));
                    },
                    "timeout"_a = nb::none());
    nb::class_<sdk::Client>(m, "Client")
            .def(
                    "create_chronicle",
                    [](sdk::Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        return pack(unwrap(call([&] { return c.createChronicle(n, d); })));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "chronicle",
                    [](sdk::Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        return pack(unwrap(call([&] { return c.getChronicle(n, d); })));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "list_chronicles",
                    [](sdk::Client& c, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto r = unwrap(call([&] { return c.listChronicles(d); }));
                        nb::list out;
                        for(auto& x: r) out.append(pack(x));
                        return out;
                    },
                    "timeout"_a = nb::none())
            .def(
                    "destroy_chronicle",
                    [](sdk::Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        check(call([&] { return c.destroyChronicle(n, d); }));
                    },
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "create_story",
                    [](sdk::Client& c, const std::string& n, const std::string& s, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        return pack(unwrap(call([&] { return c.createStory(n, s, d); })));
                    },
                    "chronicle"_a,
                    "name"_a,
                    "timeout"_a = nb::none())
            .def(
                    "story",
                    [](sdk::Client& c, uint64_t id, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        return pack(unwrap(call([&] { return c.getStory(id, d); })));
                    },
                    "id"_a,
                    "timeout"_a = nb::none())
            .def(
                    "list_stories",
                    [](sdk::Client& c, const std::string& n, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        auto r = unwrap(call([&] { return c.listStories(n, d); }));
                        nb::list out;
                        for(auto& x: r) out.append(pack(x));
                        return out;
                    },
                    "chronicle"_a,
                    "timeout"_a = nb::none())
            .def(
                    "destroy_story",
                    [](sdk::Client& c, uint64_t id, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        check(call([&] { return c.destroyStory(id, d); }));
                    },
                    "id"_a,
                    "timeout"_a = nb::none())
            .def(
                    "acquire",
                    [](sdk::Client& c, uint64_t id, const std::string& identity, std::optional<double> t)
                    {
                        auto d = deadline(t);
                        return unwrap(call([&] { return c.acquire(id, identity, d); }));
                    },
                    "id"_a,
                    "identity"_a,
                    "timeout"_a = nb::none())
            .def(
                    "read",
                    [](sdk::Client& c, uint64_t id, nb::handle start, nb::handle end, std::optional<double> t)
                    {
                        sdk::HlcRange r{hlc(start), hlc(end)};
                        auto d = deadline(t);
                        return unwrap(call([&] { return c.read(id, r, d); }));
                    },
                    "id"_a,
                    "start"_a,
                    "end"_a,
                    "timeout"_a = nb::none())
            .def(
                    "tail",
                    [](sdk::Client& c, uint64_t id, nb::object after, std::optional<double> t)
                    {
                        std::optional<sdk::Position> p;
                        if(!after.is_none())
                            p = sdk::Position{hlc(after.attr("hlc")), eventId(after.attr("id"))};
                        auto d = deadline(t);
                        return unwrap(call([&] { return c.tail(id, p, d); }));
                    },
                    "id"_a,
                    "after"_a = nb::none(),
                    "timeout"_a = nb::none());
    m.def("connect",
          [](const std::string& catalog,
             const std::string& player,
             double timeout,
             size_t retries,
             double backoff,
             size_t inFlight,
             size_t batchSize,
             size_t maxItems,
             size_t maxBytes,
             nb::dict args)
          {
              auto d = deadline(timeout);
              sdk::ClientOptions o;
              o.catalog_endpoint = catalog;
              o.player_endpoint = player;
              o.rpc_timeout =
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeout));
              o.retry.max_retries = retries;
              if(!std::isfinite(backoff) || backoff < 0 || backoff > 3600)
                  throw nb::value_error("invalid retry_backoff");
              o.retry.backoff =
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(backoff));
              o.max_in_flight = inFlight;
              o.batch_size = batchSize;
              o.max_batch_items = maxItems;
              o.max_batch_bytes = maxBytes;
              for(auto [k, v]: args)
              {
                  auto key = nb::cast<std::string>(k);
                  if(nb::isinstance<nb::str>(v))
                      o.channel_args[key] = nb::cast<std::string>(v);
                  else
                      o.channel_args[key] = nb::cast<int>(v);
              }
              return unwrap(call([&] { return sdk::Client::Connect(std::move(o), d); }));
          });
}
