#include <ClientConfiguration.h>
#include <chronolog_client.h>

#include <chrono_monitor.h>
#include <cmd_arg_parse.h>

#include <cassert>
#include <common.h>
#include <unistd.h>

// This file is the writer part of the reader example also available in the same
// folder. Therefore, the code is responsible for creating the chronicle and
// story that the reader example will later access.
//
// For this reason, the chronicle and story are not destroyed at the end of this
// example — they must remain available for the reader to consume.

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

    // Build portal config
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[ClientExample] Starting ChronoLog Client Example");

    // Create a ChronoLog client
    chronolog::Client client(portalConf);

    /// Connect to ChronoVisor
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

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    std::cout << "[ClientExample] ReleaseStory returned: " << chronolog::to_string_client(ret) << "\n";

    // Disconnect from ChronoVisor
    ret = client.Disconnect();
    std::cout << "[ClientExample] Disconnect returned: " << chronolog::to_string_client(ret) << "\n";

    LOG_INFO("[ClientExample] Finished successfully");
    return 0;
}