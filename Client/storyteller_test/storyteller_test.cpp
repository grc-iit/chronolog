#include <iostream>
#include <ctime>
#include <chrono>
#include <unistd.h>

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

int main(int argc, char **argv)
{
//    if (argc != 3)
//    {
//        std::cerr << "Usage: " << argv[0] << " <address> <provider_id>" << std::endl;
//        exit(0);
//    }
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    std::string proto_conf = confManager.RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
    tl::engine myEngine(proto_conf, THALLIUM_CLIENT_MODE);
    tl::remote_procedure record_event = myEngine.define("record_event");
    std::string visor_uri = proto_conf + "://"
                            + confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string() + ":"
                            + std::to_string(confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT);
    tl::endpoint server = myEngine.lookup(visor_uri);
    uint16_t provider_id = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.SERVICE_PROVIDER_ID;



//    chronolog::StorytellerClient storytellerClient(myEngine, argv[1], provider_id);


    for (int i = 0; i < 10; ++i)
    {
        sleep(10);
        uint64_t log_time = get_chrono_timestamp();
        chl::LogEvent event(1, log_time, 7, i, "line_" + std::to_string(i));
        std::cout << "generated_event {" << event.storyId << ":" << event.time() << ":" << event.clientId << ":"
                  << event.index() << "}" << std::endl;
//        recordingClient->send_event_msg(event);
    }


    return 0;
}
