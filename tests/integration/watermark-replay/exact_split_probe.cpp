// Client side of tests/integration/watermark_exact_split_test.sh: writes a burst
// of events and prints the timestamp of each, or replays a story and prints
// every event it returns, so the script can compare the two by identity.
//
//   write <chronicle> <story> <count> [hold_secs]
//       acquires the story, logs <count> events and prints WROTE <timestamp>
//       per event, then WRITTEN; keeps the story acquired for hold_secs
//       (default 0), releases it, and prints WRITE_STATUS <CL_...>. Holding
//       keeps the story active, so another client can acquire it without the
//       visor notifying the grapher -- which would block while the grapher is
//       stopped.
//   replay <chronicle> <story>
//       acquires the story, replays its whole range, releases it, and prints
//       EVENT <timestamp> <client id> <index> per event, then
//       REPLAY_STATUS <CL_...> and REPLAY_COUNT <n>
//
// Usage: exact_split_probe --config <client conf> write|replay <chronicle> <story> [count [hold_secs]]
// Arguments are positional: the shared cmd_arg_parse getopt rejects flags it
// does not know.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>

int main(int argc, char** argv)
{
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty() && !confManager.load_from_file(conf_file_path))
    {
        std::cerr << "[ExactSplitProbe] Failed to load configuration file '" << conf_file_path << "'" << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<std::string> positional;
    for(int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if(arg == "--config" || arg == "-c")
        {
            ++i; // skip the config path value
            continue;
        }
        positional.push_back(arg);
    }
    if(positional.size() < 3 || (positional[0] != "write" && positional[0] != "replay") ||
       (positional[0] == "write" && positional.size() < 4))
    {
        std::cerr << "Usage: " << argv[0] << " --config <client conf> write|replay <chronicle> <story> [count]"
                  << std::endl;
        return EXIT_FAILURE;
    }
    std::string const mode = positional[0];
    std::string const chronicle_name = positional[1];
    std::string const story_name = positional[2];

    if(chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                             confManager.LOG_CONF.LOGFILE,
                                             confManager.LOG_CONF.LOGLEVEL,
                                             confManager.LOG_CONF.LOGNAME,
                                             confManager.LOG_CONF.LOGFILESIZE,
                                             confManager.LOG_CONF.LOGFILENUM,
                                             confManager.LOG_CONF.FLUSHLEVEL) == 1)
    {
        return EXIT_FAILURE;
    }

    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP = confManager.QUERY_CONF.IP;
    queryConf.PORT = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    chronolog::Client client(portalConf, queryConf);
    int ret = client.Connect();
    if(ret != chronolog::CL_SUCCESS)
    {
        std::cout << (mode == "write" ? "WRITE_STATUS " : "REPLAY_STATUS ") << chronolog::to_string_client(ret)
                  << std::endl;
        return EXIT_FAILURE;
    }

    if(mode == "write")
    {
        // the chronicle may exist from an earlier burst
        client.CreateChronicle(chronicle_name);
        auto acquire_result = client.AcquireStory(chronicle_name, story_name);
        if(acquire_result.first != chronolog::CL_SUCCESS)
        {
            std::cout << "WRITE_STATUS " << chronolog::to_string_client(acquire_result.first) << std::endl;
            client.Disconnect();
            return EXIT_FAILURE;
        }
        int const count = std::stoi(positional[3]);
        for(int i = 0; i < count; ++i)
        {
            uint64_t const timestamp = acquire_result.second->log_event("exact-split event " + std::to_string(i));
            std::cout << "WROTE " << timestamp << std::endl;
        }
        std::cout << "WRITTEN" << std::endl;
        int const hold_secs = (positional.size() > 4) ? std::stoi(positional[4]) : 0;
        std::this_thread::sleep_for(std::chrono::seconds(hold_secs));
        client.ReleaseStory(chronicle_name, story_name);
        client.Disconnect();
        std::cout << "WRITE_STATUS CL_SUCCESS" << std::endl;
        return EXIT_SUCCESS;
    }

    auto acquire_result = client.AcquireStory(chronicle_name, story_name);
    if(acquire_result.first != chronolog::CL_SUCCESS)
    {
        std::cout << "REPLAY_STATUS " << chronolog::to_string_client(acquire_result.first) << std::endl;
        client.Disconnect();
        return EXIT_FAILURE;
    }
    std::vector<chronolog::Event> events;
    ret = client.ReplayStory(chronicle_name, story_name, 1, 2000000000000000000ULL, events);
    for(auto const& event: events)
    {
        std::cout << "EVENT " << event.time() << " " << event.client_id() << " " << event.index() << std::endl;
    }
    std::cout << "REPLAY_STATUS " << chronolog::to_string_client(ret) << std::endl;
    std::cout << "REPLAY_COUNT " << events.size() << std::endl;
    client.ReleaseStory(chronicle_name, story_name);
    client.Disconnect();
    return (ret == chronolog::CL_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
