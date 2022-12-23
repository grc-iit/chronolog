#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

namespace tl = thallium;

int main(int argc, char** argv) {
    if(argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <address> <provider_id>" << std::endl;
        exit(0);
    }
    tl::engine myEngine("tcp", THALLIUM_CLIENT_MODE);
    tl::remote_procedure record_event  = myEngine.define("record_event");
    tl::remote_procedure hello = myEngine.define("hello").disable_response();
    tl::endpoint server = myEngine.lookup(argv[1]);
    uint16_t provider_id = atoi(argv[2]);
    tl::provider_handle ph(server, provider_id);
    std::string name("Storyteller:Inna");
    hello.on(ph)(name);
    int ret = record_event.on(ph)("story_line_1");
    std::cout << "(record_event) response " << ret << std::endl;

    return 0;
}
