
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <chronolog_client.h>


using chronolog::ClientPortalServiceConf;
void BindChronologClientPortalServiceConf(pybind11::module& m)
{
    pybind11::class_<ClientPortalServiceConf>(m, "ClientPortalServiceConf")
            .def(pybind11::init<const std::string&, const std::string&, uint16_t, uint16_t>());
};

using chronolog::ClientQueryServiceConf;
void BindChronologClientQueryServiceConf(pybind11::module& m)
{
    pybind11::class_<ClientQueryServiceConf>(m, "ClientQueryServiceConf")
            .def(pybind11::init<const std::string&, const std::string&, uint16_t, uint16_t>());
};


using chronolog::StoryHandle;
using chronolog::LogEventFuture;
using chronolog::BoundedLogEventAppender;
using chronolog::PerKeeperBoundedLogEventAppender;
using chronolog::PackedReplayBatch;

void BindChronologLogEventFuture(pybind11::module& m)
{
    pybind11::class_<LogEventFuture>(m, "LogEventFuture")
            .def("valid", &LogEventFuture::valid)
            .def("future_count", &LogEventFuture::future_count)
            .def("wait", &LogEventFuture::wait);
}

class PyStoryHandle: public StoryHandle
{
public:
    /* Inherit the constructors */
    using StoryHandle::StoryHandle;

    /* Trampoline (need one for each virtual function) */
    uint64_t log_event(std::string const& event_string) override
    {
        PYBIND11_OVERRIDE_PURE(uint64_t,    /* Return type */
                               StoryHandle, /* Parent class */
                               log_event,   /* Name of function in C++ (must match Python name) */
                               event_string /* Argument(s) */
        );
    }

    int replay_tail_incremental(uint64_t end_time, std::vector<chronolog::Event>& events) override
    {
        PYBIND11_OVERRIDE(int,                   /* Return type */
                          StoryHandle,           /* Parent class */
                          replay_tail_incremental, /* Name of function in C++ (must match Python name) */
                          end_time,              /* Argument(s) */
                          events);
    }

    int replay_tail_incremental_packed(uint64_t end_time, chronolog::PackedReplayBatch& events) override
    {
        PYBIND11_OVERRIDE(int,
                          StoryHandle,
                          replay_tail_incremental_packed,
                          end_time,
                          events);
    }
};

void BindChronologStoryHandle(pybind11::module& m)
{
    pybind11::class_<StoryHandle, PyStoryHandle>(m, "StoryHandle")
            .def(pybind11::init<>())
            .def("log_event", &StoryHandle::log_event, pybind11::arg("log_string"))
            .def("log_event_async", &StoryHandle::log_event_async, pybind11::arg("log_string"))
            .def("log_events_async", &StoryHandle::log_events_async, pybind11::arg("log_strings"))
            .def("log_events_async_owned", &StoryHandle::log_events_async_owned, pybind11::arg("log_strings"))
            .def("log_events_bounded",
                 &StoryHandle::log_events_bounded,
                 pybind11::arg("log_strings"),
                 pybind11::arg("batch_size") = 1,
                 pybind11::arg("max_outstanding") = 1)
            .def("log_events_bounded_per_keeper",
                 &StoryHandle::log_events_bounded_per_keeper,
                 pybind11::arg("log_strings"),
                 pybind11::arg("keeper_batch_size") = 1,
                 pybind11::arg("max_outstanding_futures") = 1)
            .def("make_per_keeper_bounded_appender",
                 &StoryHandle::make_per_keeper_bounded_appender,
                 pybind11::arg("keeper_batch_size") = 1,
                 pybind11::arg("max_outstanding_futures") = 1)
            .def("replay_tail_incremental",
                 &StoryHandle::replay_tail_incremental,
                 pybind11::arg("end_time"),
                 pybind11::arg("events"))
            .def("replay_tail_incremental_packed",
                 &StoryHandle::replay_tail_incremental_packed,
                 pybind11::arg("end_time"),
                 pybind11::arg("events"));
};

void BindChronologBoundedLogEventAppender(pybind11::module& m)
{
    pybind11::class_<PerKeeperBoundedLogEventAppender>(m, "PerKeeperBoundedLogEventAppender")
            .def("append", &PerKeeperBoundedLogEventAppender::append, pybind11::arg("log_string"))
            .def("append_many", &PerKeeperBoundedLogEventAppender::append_many, pybind11::arg("log_strings"))
            .def("flush", &PerKeeperBoundedLogEventAppender::flush)
            .def("future_count", &PerKeeperBoundedLogEventAppender::future_count)
            .def("future_count_max_per_call", &PerKeeperBoundedLogEventAppender::future_count_max_per_call)
            .def("future_wait_count", &PerKeeperBoundedLogEventAppender::future_wait_count)
            .def("future_wait_ns", &PerKeeperBoundedLogEventAppender::future_wait_ns)
            .def("future_wait_max_ns", &PerKeeperBoundedLogEventAppender::future_wait_max_ns);

    pybind11::class_<BoundedLogEventAppender>(m, "BoundedLogEventAppender")
            .def(pybind11::init<StoryHandle&, std::size_t, std::size_t>(),
                 pybind11::arg("story_handle"),
                 pybind11::arg("batch_size") = 1,
                 pybind11::arg("max_outstanding") = 1,
                 pybind11::keep_alive<1, 2>())
            .def("append", &BoundedLogEventAppender::append, pybind11::arg("log_string"))
            .def("append_many", &BoundedLogEventAppender::append_many, pybind11::arg("log_strings"))
            .def("flush", &BoundedLogEventAppender::flush);
};

PYBIND11_MAKE_OPAQUE(std::pair<int, StoryHandle>);

using chronolog::Event;
void BindChronologEvent(pybind11::module& m)
{
    pybind11::class_<Event>(m, "Event")
            .def(pybind11::init<uint64_t, uint64_t, uint32_t, const std::string&>())
            .def("time", &Event::time)
            .def("client_id", &Event::client_id)
            .def("index", &Event::index)
            .def("log_record", &Event::log_record);
};

PYBIND11_MAKE_OPAQUE(std::vector<Event>);

void BindChronologEventVector(pybind11::module& m) { pybind11::bind_vector<std::vector<Event>>(m, "EventList"); };

void BindChronologPackedReplayBatch(pybind11::module& m)
{
    pybind11::class_<PackedReplayBatch>(m, "PackedReplayBatch")
            .def(pybind11::init<>())
            .def("clear", &PackedReplayBatch::clear)
            .def("event_count", &PackedReplayBatch::event_count)
            .def("payload_bytes", &PackedReplayBatch::payload_bytes)
            .def("time", &PackedReplayBatch::time, pybind11::arg("index"))
            .def("client_id", &PackedReplayBatch::client_id, pybind11::arg("index"))
            .def("index", &PackedReplayBatch::event_index, pybind11::arg("index"))
            .def("payload_size", &PackedReplayBatch::payload_size, pybind11::arg("index"))
            .def("payload", &PackedReplayBatch::payload, pybind11::arg("index"));
};

using chronolog::Client;

void BindChronologClient(pybind11::module& m)
{
    pybind11::class_<Client>(m, "Client")
            .def(pybind11::init<const ClientPortalServiceConf&>())
            .def(pybind11::init<const ClientPortalServiceConf&, const ClientQueryServiceConf&>())
            .def("Connect", &Client::Connect)
            .def("Disconnect", &Client::Disconnect)
            .def("CreateChronicle", &Client::CreateChronicle)
            .def("DestroyChronicle", &Client::DestroyChronicle)
            .def("AcquireStory", &Client::AcquireStory, pybind11::return_value_policy::reference)
            .def("ReleaseStory", &Client::ReleaseStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"))
            .def("DestroyStory", &Client::DestroyStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"))
            .def("ReplayStory", &Client::ReplayStory);
};

PYBIND11_MODULE(py_chronolog_client, m)
{
    BindChronologClientPortalServiceConf(m);
    BindChronologClientQueryServiceConf(m);
    BindChronologLogEventFuture(m);
    BindChronologStoryHandle(m);
    BindChronologBoundedLogEventAppender(m);
    BindChronologEvent(m);
    BindChronologEventVector(m);
    BindChronologPackedReplayBatch(m);
    BindChronologClient(m);
}
