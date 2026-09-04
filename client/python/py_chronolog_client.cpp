
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <chronolog_client.h>

// Declared before any binding that mentions std::vector<Event>.
// PYBIND11_MAKE_OPAQUE specializes pybind11::detail::type_caster for the type.
// Declaring it after a .def() that names the type is ill-formed-no-diagnostic-
// required: GCC picks the specialization anyway because it instantiates at end
// of translation unit, but a compiler that instantiates at the point of use
// would select the generic list_caster instead -- binding the out-parameter by
// copy, so it is never written back. Keeping the declaration first removes the
// portability hazard rather than relying on instantiation timing.
PYBIND11_MAKE_OPAQUE(std::vector<chronolog::Event>);


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

    int playback(size_t n, std::vector<chronolog::Event>& events) override
    {
        PYBIND11_OVERRIDE(int,         /* Return type */
                          StoryHandle, /* Parent class */
                          playback,    /* Name of function in C++ (must match Python name) */
                          n,           /* Argument(s) */
                          events);
    }
};

void BindChronologStoryHandle(pybind11::module& m)
{
    pybind11::class_<StoryHandle, PyStoryHandle>(m, "StoryHandle")
            .def(pybind11::init<>())
            .def("log_event", &StoryHandle::log_event, pybind11::arg("log_string"))
            // Tail read: fill `events` (an EventList) with the most recent `n`
            // events of this story, served from the keepers' in-memory tail.
            // Returns CL_SUCCESS on success; events are in ascending order.
            .def("playback", &StoryHandle::playback, pybind11::arg("n"), pybind11::arg("events"));
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


void BindChronologEventVector(pybind11::module& m) { pybind11::bind_vector<std::vector<Event>>(m, "EventList"); };

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
            .def("ShowChronicles", &Client::ShowChronicles)
            .def("ShowStories", &Client::ShowStories, pybind11::arg("chronicle_name"))
            .def("ReplayStory",
                 pybind11::overload_cast<std::string const&,
                                         std::string const&,
                                         uint64_t,
                                         uint64_t,
                                         std::vector<chronolog::Event>&>(&Client::ReplayStory));
};

PYBIND11_MODULE(py_chronolog_client, m)
{
    BindChronologClientPortalServiceConf(m);
    BindChronologClientQueryServiceConf(m);
    // Event, then EventList (bind_vector needs Event registered), then anything
    // whose signature mentions EventList. pybind renders a parameter's type name
    // at .def() time, so a binding registered before EventList exists shows the
    // raw C++ type in its signature and help text instead of EventList.
    BindChronologEvent(m);
    BindChronologEventVector(m);
    BindChronologStoryHandle(m);
    BindChronologClient(m);
}
