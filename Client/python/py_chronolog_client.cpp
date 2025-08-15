
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <chronolog_client.h>



using chronolog::ClientPortalServiceConf;
void BindChronologClientPortalServiceConf(pybind11::module &m) 
{
   pybind11::class_<ClientPortalServiceConf>(m,"ClientPortalServiceConf")
      .def(pybind11::init<const std::string &, const std::string &, uint16_t, uint16_t>())
    ;
};

using chronolog::ClientQueryServiceConf;
void BindChronologClientQueryServiceConf(pybind11::module &m) 
{
   pybind11::class_<ClientQueryServiceConf>(m,"ClientQueryServiceConf")
      .def(pybind11::init<const std::string &, const std::string &, uint16_t, uint16_t>())
    ;
};


using chronolog::StoryHandle;
class PyStoryHandle : public StoryHandle 
{
public:
    /* Inherit the constructors */
    using StoryHandle::StoryHandle;

    /* Trampoline (need one for each virtual function) */
    uint64_t log_event(std::string const & event_string) override {
        PYBIND11_OVERRIDE_PURE(
            uint64_t, /* Return type */
            StoryHandle,      /* Parent class */
            log_event,          /* Name of function in C++ (must match Python name) */
            event_string      /* Argument(s) */
        );
    }
};

void BindChronologStoryHandle(pybind11::module &m)
{
   pybind11::class_<StoryHandle,PyStoryHandle>(m, "StoryHandle")
    .def(pybind11::init<>())
    .def("log_event",&StoryHandle::log_event, pybind11::arg("log_string") );

};

PYBIND11_MAKE_OPAQUE(std::pair<int,StoryHandle>);

using chronolog::Event;
void BindChronologEvent(pybind11::module & m)
{
    pybind11::class_<Event>(m,"Event")
    .def(pybind11::init< uint64_t, uint64_t, uint32_t, const std::string &>())
    .def("time",&Event::time)
    .def("client_id",&Event::client_id)
    .def("index",&Event::index)
    .def("log_record",&Event::log_record);

};

PYBIND11_MAKE_OPAQUE(std::vector<Event>);

void BindChronologEventVector(pybind11::module &m)
{
    pybind11::bind_vector<std::vector<Event>>(m, "EventList");
};

using chronolog::Client;

void BindChronologClient(pybind11::module &m)
{
   pybind11::class_<Client>(m,"Client")
    .def(pybind11::init<const ClientPortalServiceConf &>())
    .def(pybind11::init<const ClientPortalServiceConf & , const ClientQueryServiceConf & >())
    .def("Connect", &Client::Connect)
    .def("Disconnect", &Client::Disconnect)
    .def("CreateChronicle", &Client::CreateChronicle)
    .def("DestroyChronicle", &Client::DestroyChronicle)
    .def("AcquireStory", &Client::AcquireStory, pybind11::return_value_policy::reference)
    .def("ReleaseStory", &Client::ReleaseStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"))
    .def("DestroyStory", &Client::DestroyStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"))
    .def("ReplayStory", &Client::ReplayStory)
    ;
};

PYBIND11_MODULE(py_chronolog_client, m) 
{
    BindChronologClientPortalServiceConf(m);
    BindChronologClientQueryServiceConf(m);
    BindChronologStoryHandle(m);
    BindChronologEvent(m);
    BindChronologEventVector(m);
    BindChronologClient(m);
}
