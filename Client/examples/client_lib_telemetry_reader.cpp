#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <cmd_arg_parse.h>
#include <iostream>
#include <cassert>
#include <getopt.h>

int main(int argc, char** argv) {
    std::string config_file, chronicle_name, story_name;
    uint64_t segment_start = 0, segment_end = 0;

    // Option parsing
    int opt;
    struct option long_options[] = {
        {"config", required_argument, nullptr, 'c'},
        {"chronicle", required_argument, nullptr, 'C'},
        {"story", required_argument, nullptr, 's'},
        {"start", required_argument, nullptr, 'S'},
        {"end", required_argument, nullptr, 'E'},
        {nullptr, 0, nullptr, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:C:s:S:E:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'C': chronicle_name = optarg; break;
            case 's': story_name = optarg; break;
            case 'S': segment_start = std::stoull(optarg); break;
            case 'E': segment_end = std::stoull(optarg); break;
            default: std::cerr << "Invalid option\n"; return -1;
        }
    }

    // Load config
    chronolog::ClientConfiguration conf;
    if (!conf.load_from_file(config_file)) {
        std::cerr << "[ReaderClient] Failed to load config.\n";
        return -1;
    }

    // Setup portal + query services
    chronolog::Client client(conf.PORTAL_CONF, conf.QUERY_CONF);
    assert(client.Connect() == chronolog::CL_SUCCESS);

    std::vector<chronolog::Event> replay_events;
    int ret = client.ReplayStory(chronicle_name, story_name, segment_start, segment_end, replay_events);

    if (ret == chronolog::CL_SUCCESS) {
        std::cout << "[ReaderClient] Retrieved " << replay_events.size() << " events:\n";
        for (const auto& e : replay_events) {
            std::cout << e.to_string() << std::endl;
        }
    } else {
        std::cerr << "[ReaderClient] Failed to replay story. Error: " << ret << std::endl;
    }

    client.ReleaseStory(chronicle_name, story_name);
    client.Disconnect();
    return 0;
}