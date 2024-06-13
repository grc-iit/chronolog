
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chronolog_client.h>



using chronolog::ClientPortalServiceConf;
using chronolog::Client;

void BindChronologClientPortalServiceConf(pybind11::module &m) 
{
   pybind11::class_<ClientPortalServiceConf>(m,"ClientPortalServiceConf")
      .def(pybind11::init<const std::string &, const std::string &, uint16_t, uint16_t>())
      .def("proto_conf", &ClientPortalServiceConf::proto_conf)
      .def("ip", &ClientPortalServiceConf::ip)
      .def("port", &ClientPortalServiceConf::port)
      .def("provider_id", &ClientPortalServiceConf::provider_id);
};

using chronolog::StoryHandle;
class PyStoryHandle : public StoryHandle 
{
public:
    /* Inherit the constructors */
    using StoryHandle::StoryHandle;

    /* Trampoline (need one for each virtual function) */
    int log_event(std::string const & event_string) override {
        PYBIND11_OVERRIDE_PURE(
            int, /* Return type */
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

void BindChronologClient(pybind11::module &m)
{
   pybind11::class_<Client>(m,"Client")
    .def(pybind11::init<const ClientPortalServiceConf &>())
    .def("Connect", &Client::Connect)
    .def("Disconnect", &Client::Disconnect)
    .def("CreateChronicle", &Client::CreateChronicle)
    .def("DestroyChronicle", &Client::DestroyChronicle)
    .def("AcquireStory", &Client::AcquireStory, pybind11::return_value_policy::reference)
    .def("ReleaseStory", &Client::ReleaseStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"))
    .def("DestroyStory", &Client::DestroyStory, pybind11::arg("chronicle_name"), pybind11::arg("story_name"));

};

PYBIND11_MODULE(py_chronolog_client, m) 
{
    BindChronologClientPortalServiceConf(m);
    BindChronologStoryHandle(m);
    BindChronologClient(m);
}
