// Replay probe for the watermark-split integration test: acquires a story,
// replays a time range through the player (archive portion below the split
// boundary B, keeper hot fetch above it), and prints machine-parseable totals.
// Never destroys anything, so it can probe the same story repeatedly across
// the hot -> mixed -> archived phases of an event's life.
//
//   REPLAY_STATUS <CL_...>
//   REPLAY_COUNT <events returned>
//   REPLAY_UNIQUE <distinct events among them>
//
// Usage: replay_split_check --config <client conf> [chronicle [story]]
//        (defaults: TailChronicle TailStory — the tail-reader example's story)

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
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
        std::cerr << "[ReplaySplitCheck] Failed to load configuration file '" << conf_file_path << "'" << std::endl;
        return EXIT_FAILURE;
    }

    // trailing non-flag args: chronicle name, story name
    std::string chronicle_name = "TailChronicle";
    std::string story_name = "TailStory";
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
    if(positional.size() > 0)
    {
        chronicle_name = positional[0];
    }
    if(positional.size() > 1)
    {
        story_name = positional[1];
    }

    int result = chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                                       confManager.LOG_CONF.LOGFILE,
                                                       confManager.LOG_CONF.LOGLEVEL,
                                                       confManager.LOG_CONF.LOGNAME,
                                                       confManager.LOG_CONF.LOGFILESIZE,
                                                       confManager.LOG_CONF.LOGFILENUM,
                                                       confManager.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
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
        std::cout << "REPLAY_STATUS " << chronolog::to_string_client(ret) << std::endl;
        return EXIT_FAILURE;
    }

    auto acquire_result = client.AcquireStory(chronicle_name, story_name);
    if(acquire_result.first != chronolog::CL_SUCCESS)
    {
        std::cout << "REPLAY_STATUS " << chronolog::to_string_client(acquire_result.first) << std::endl;
        client.Disconnect();
        return EXIT_FAILURE;
    }

    // full range: everything the story ever recorded
    uint64_t start_time = 1;
    uint64_t end_time = 2000000000000000000ULL;

    std::vector<chronolog::Event> events;
    ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, events);
    std::cout << "REPLAY_STATUS " << chronolog::to_string_client(ret) << std::endl;

    std::set<std::string> unique_events;
    for(auto const& event: events)
    {
        unique_events.insert(event.to_string());
    }
    std::cout << "REPLAY_COUNT " << events.size() << std::endl;
    std::cout << "REPLAY_UNIQUE " << unique_events.size() << std::endl;

    client.ReleaseStory(chronicle_name, story_name);
    client.Disconnect();
    return (ret == chronolog::CL_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
