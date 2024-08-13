#include <iostream>
#include <ctime>
#include <chrono>
#include <unistd.h>
#include "chrono_monitor.h"

#include <thallium/serialization/stl/string.hpp>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "chronolog_client.h"

namespace tl = thallium;
namespace chl = chronolog;

uint64_t get_chrono_timestamp()
{
    return std::chrono::system_clock::now().time_since_epoch().count();
}

int main(int argc, char**argv)
{
    if(argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <address> <provider_id>" << std::endl;
        exit(0);
    }
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
    tl::remote_procedure record_event = myEngine.define("record_event");
    tl::endpoint server = myEngine.lookup(argv[1]);
    uint16_t provider_id = atoi(argv[2]);



//    chronolog::StorytellerClient storytellerClient(myEngine, argv[1], provider_id);


    for(int i = 0; i < 10; ++i)
    {
        sleep(10);
        uint64_t log_time = get_chrono_timestamp();
        chl::LogEvent event(1, log_time, 7, i, "line_" + std::to_string(i));
        LOG_DEBUG("generated_event {}:{}:{}:{}", event.storyId, event.time(), event.clientId, event.index());
        /*std::cout << "generated_event {" << event.storyId << ":" << event.time() << ":" << event.clientId << ":"
                          << event.index() << "}" << std::endl;*/

//        recordingClient->send_event_msg(event);
    }


    return 0;
}
