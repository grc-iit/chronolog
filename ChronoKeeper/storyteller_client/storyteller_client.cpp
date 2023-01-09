#include <iostream>
#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

namespace tl = thallium;

int main(int argc, char** argv) {
    if(argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <address> <provider_id>" << std::endl;
        exit(0);
    }
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
    tl::remote_procedure record_event  = myEngine.define("record_event");
    tl::remote_procedure hello = myEngine.define("hello").disable_response();
    tl::remote_procedure record_int_values= myEngine.define("record_int_values");
    tl::endpoint server = myEngine.lookup(argv[1]);
    uint16_t provider_id = atoi(argv[2]);
    tl::provider_handle ph(server, provider_id);

    std::string name("Storyteller:Inna");
    hello.on(ph)(name);


    int int_ret = record_int_values.on(ph)(123,456);
    std::cout << "(record_value (123) response " << int_ret << std::endl;
    try   // timed request throws timeout exception, cath it and retry... 
    {
    int_ret = record_int_values.on(ph).timed(std::chrono::milliseconds(3),123,456);
    std::cout << "(record_value (123) timed response " << int_ret << std::endl;
    }
    catch(thallium::timeout const&) 
    { std::cout << "(record_value (123) timed (3mls) :timeout" << std::endl; }

   
    std::string log_record("story_line_1");
    int ret = record_event.on(ph)(static_cast<uint64_t>(123), static_cast<uint64_t>(321), log_record);
    std::cout << "(record_event) response " << ret << std::endl;
    return 0;
}
