#include <cassert>
#include <common.h>
#include <unistd.h>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>


int main(int argc, char** argv)
{

    // Load configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            std::cerr << "[ClientExample] Failed to load configuration file '" << conf_file_path
                      << "'. Using default values instead." << std::endl;
        }
        else
        {
            std::cout << "[ClientExample] Configuration file loaded successfully from '" << conf_file_path << "'."
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[ClientExample] No configuration file provided. Using "
                     "default values."
                  << std::endl;
    }
    confManager.log_configuration(std::cout);

    // Initialize logging
    int result = chronolog::chrono_monitor::initialize(
            confManager.LOG_CONF.LOGTYPE, confManager.LOG_CONF.LOGFILE, confManager.LOG_CONF.LOGLEVEL,
            confManager.LOG_CONF.LOGNAME, confManager.LOG_CONF.LOGFILESIZE, confManager.LOG_CONF.LOGFILENUM,
            confManager.LOG_CONF.FLUSHLEVEL);
    if(result == 1) { return EXIT_FAILURE; }

    // Build portal configs
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    // Build query configs
    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP = confManager.QUERY_CONF.IP;
    queryConf.PORT = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    LOG_INFO("[ClientExample] Starting ChronoLog Client Example");

    // Create a ChronoLog client
    chronolog::Client client(portalConf, queryConf);

    // Connect to ChronoVisor
    int ret = client.Connect();
    std::cout << "[ClientExample] Connect returned: " << chronolog::to_string_client(ret) << "\n";

    // Create a chronicle
    std::string chronicle_name = "MyChronicle";
    std::map<std::string, std::string> chronicle_attrs;
    int flags = 0;
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    std::cout << "[ClientExample] CreateChronicle returned: " << chronolog::to_string_client(ret) << "\n";

    // Acquire a story
    std::string story_name = "MyStory";
    std::map<std::string, std::string> story_attrs;
    auto acquire_result = client.AcquireStory(chronicle_name, story_name, story_attrs, flags);
    std::cout << "[ClientExample] AcquireStory returned: " << chronolog::to_string_client(acquire_result.first) << "\n";
    auto story_handle = acquire_result.second;

    // Log a few events to the story
    // Event values are hardcoded for example purposes.
    story_handle->log_event("Event 1");
    story_handle->log_event("Event 2");
    story_handle->log_event("Event 3");

    // Temporary workaround: wait to ensure data is persisted before reading.
    // This sleep duration allows background processes to complete the required
    // steps for persistence
    // The exact time may vary depending on the system configuration.
    // Future versions may revise or eliminate the need for this hardcoded delay.
    sleep(120);

    // Use an intentionally wide time range to ensure all events are captured.
    // 'start_time' is set to 1 to include any valid timestamp from the beginning.
    // 'end_time' is set to a very large value to ensure we read up to the latest
    // events. This approach avoids missing any data due to timing uncertainties.
    uint64_t start_time = 1;
    uint64_t end_time = 2000000000000000000;

    // Read a story
    std::vector<chronolog::Event> events;
    ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, events);
    std::cout << "[ClientExample] ReplayStory returned: " << chronolog::to_string_client(ret) << "\n";

    if(ret == chronolog::CL_SUCCESS)
    {
        std::cout << "[ClientExample] Replay succeeded with " << events.size() << " event(s):\n";
        for(const auto& ev: events) { std::cout << ev.to_string() << "\n"; }
    }

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    std::cout << "[ClientExample] ReleaseStory returned: " << chronolog::to_string_client(ret) << "\n";

    // Destroy the story
    ret = client.DestroyStory(chronicle_name, story_name);
    std::cout << "[ClientExample] DestroyStory returned: " << chronolog::to_string_client(ret) << "\n";

    // Destroy the chronicle
    ret = client.DestroyChronicle(chronicle_name);
    std::cout << "[ClientExample] DestroyChronicle returned: " << chronolog::to_string_client(ret) << "\n";

    // Disconnect from ChronoVisor
    ret = client.Disconnect();
    std::cout << "[ClientExample] Disconnect returned: " << chronolog::to_string_client(ret) << "\n";

    LOG_INFO("[ClientExample] Finished successfully");
    return 0;
}