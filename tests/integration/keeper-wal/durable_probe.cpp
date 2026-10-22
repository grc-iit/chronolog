// Client side of tests/integration/keeper_wal_crash_test.sh: writes events
// durable or accepted and prints the identity of each one the keeper
// acknowledged, or replays a story and prints the identity of every event it
// returns, so the script can compare the two.
//
//   --write <n> --mode durable|accepted
//       acquires the story, logs <n> events through
//       log_event(record, options, event_time) and prints
//       "<time> <client id> <index>" for each one answered CL_SUCCESS. After
//       the last call it prints LAST_ACK_NS <wall clock ns>, then releases the
//       story and prints WRITE_ACKED <acked> <n> and one
//       WRITE_CODE <CL_...> <count> per return code.
//   --replay
//       acquires the story, replays its whole range, releases it, and prints
//       "<time> <client id> <index>" per returned event, then
//       REPLAY_STATUS <CL_...> and REPLAY_COUNT <n>.
//
// Usage: durable_probe --config <client conf> --chronicle <name> --story <name>
//                      (--write <n> --mode durable|accepted | --replay)
//
// The client API does not return an event's index. This client is new and logs
// nothing else, and every call it makes reaches the point where the client
// takes the next index (the probe stops at the first call that returns before
// it), so the i-th call (from 1) carries index i.

#include <chrono>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>

namespace
{

void usage(char const* name)
{
    std::cerr << "Usage: " << name
              << " --config <client conf> --chronicle <name> --story <name>"
                 " (--write <n> --mode durable|accepted | --replay)"
              << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
    std::string conf_file_path;
    std::string chronicle_name;
    std::string story_name;
    std::string mode;
    long write_count = -1;
    bool replay = false;

    struct option long_options[] = {{"config", required_argument, nullptr, 'c'},
                                    {"chronicle", required_argument, nullptr, 'r'},
                                    {"story", required_argument, nullptr, 's'},
                                    {"write", required_argument, nullptr, 'w'},
                                    {"mode", required_argument, nullptr, 'm'},
                                    {"replay", no_argument, nullptr, 'p'},
                                    {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "c:", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                conf_file_path = optarg;
                break;
            case 'r':
                chronicle_name = optarg;
                break;
            case 's':
                story_name = optarg;
                break;
            case 'w':
                write_count = std::strtol(optarg, nullptr, 10);
                break;
            case 'm':
                mode = optarg;
                break;
            case 'p':
                replay = true;
                break;
            default:
                usage(argv[0]);
                return EXIT_FAILURE;
        }
    }
    bool const write = (write_count >= 0);
    if(chronicle_name.empty() || story_name.empty() || write == replay ||
       (write && mode != "durable" && mode != "accepted"))
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty() && !confManager.load_from_file(conf_file_path))
    {
        std::cerr << "[DurableProbe] Failed to load configuration file '" << conf_file_path << "'" << std::endl;
        return EXIT_FAILURE;
    }
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

    std::string const status_key = write ? "WRITE_STATUS " : "REPLAY_STATUS ";
    chronolog::Client client(portalConf, queryConf);
    int ret = client.Connect();
    if(ret != chronolog::CL_SUCCESS)
    {
        std::cout << status_key << chronolog::to_string_client(ret) << std::endl;
        return EXIT_FAILURE;
    }

    if(write)
    {
        // the chronicle may exist from an earlier run
        client.CreateChronicle(chronicle_name);
    }
    auto acquire_result = client.AcquireStory(chronicle_name, story_name);
    if(acquire_result.first != chronolog::CL_SUCCESS)
    {
        std::cout << status_key << chronolog::to_string_client(acquire_result.first) << std::endl;
        client.Disconnect();
        return EXIT_FAILURE;
    }

    if(replay)
    {
        std::vector<chronolog::Event> events;
        ret = client.ReplayStory(chronicle_name, story_name, 1, 2000000000000000000ULL, events);
        for(auto const& event: events)
        {
            std::cout << event.time() << " " << event.client_id() << " " << event.index() << std::endl;
        }
        std::cout << "REPLAY_STATUS " << chronolog::to_string_client(ret) << std::endl;
        std::cout << "REPLAY_COUNT " << events.size() << std::endl;
        client.ReleaseStory(chronicle_name, story_name);
        client.Disconnect();
        return (ret == chronolog::CL_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    chronolog::LogEventOptions options;
    options.durability = (mode == "durable") ? chronolog::Durability::Durable : chronolog::Durability::Accepted;
    chronolog::ClientId const client_id = client.client_id();
    std::map<int, long> codes;
    long acked = 0;
    bool stopped_early = false;
    for(long i = 1; i <= write_count; ++i)
    {
        uint64_t event_time = 0;
        int const code = acquire_result.second->log_event(mode + " event " + std::to_string(i), options, event_time);
        ++codes[code];
        if(code == chronolog::CL_SUCCESS)
        {
            ++acked;
            std::cout << event_time << " " << client_id << " " << i << std::endl;
        }
        else if(code == chronolog::CL_ERR_NO_KEEPERS || code == chronolog::CL_ERR_INVALID_ARG)
        {
            // returned before the client took an index: later indexes would be off by one
            stopped_early = true;
            break;
        }
    }
    auto const last_ack_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    std::cout << "LAST_ACK_NS " << last_ack_ns << std::endl;

    client.ReleaseStory(chronicle_name, story_name);
    client.Disconnect();
    std::cout << "WRITE_ACKED " << acked << " " << write_count << std::endl;
    for(auto const& [code, count]: codes)
    {
        std::cout << "WRITE_CODE " << chronolog::to_string_client(code) << " " << count << std::endl;
    }
    std::cout << status_key << (stopped_early ? "STOPPED_EARLY" : "DONE") << std::endl;
    return (!stopped_early && acked == write_count) ? EXIT_SUCCESS : EXIT_FAILURE;
}
