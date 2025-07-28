#include "ClientConfiguration.h"
#include "cmd_arg_parse.h"
#include <chronolog_client.h>
#include "chrono_monitor.h"
#include <common.h>
#include <cassert>

int main(int argc, char** argv) {
    // Load configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ReaderExample] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ReaderExample] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ReaderExample] No configuration file provided. Using default values." << std::endl;
    }
    confManager.log_configuration(std::cout);

    // Initialize logging
    int result = chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                                       confManager.LOG_CONF.LOGFILE,
                                                       confManager.LOG_CONF.LOGLEVEL,
                                                       confManager.LOG_CONF.LOGNAME,
                                                       confManager.LOG_CONF.LOGFILESIZE,
                                                       confManager.LOG_CONF.LOGFILENUM,
                                                       confManager.LOG_CONF.FLUSHLEVEL);
    if (result == 1) {
        return EXIT_FAILURE;
    }

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

    LOG_INFO("[ReaderExample] Starting ChronoLog Client Reader Example");

    // Create a ChronoLog client (reader-enabled)
    chronolog::Client client(portalConf, queryConf);

    // Connect to ChronoVisor
    int ret = client.Connect();
    assert(ret == chronolog::CL_SUCCESS);

    // Acquire the story
    std::string chronicle_name = "MyChronicle";
    std::string story_name = "MyStory";
    std::map<std::string, std::string> story_attrs;
    int flags = 0;
    auto acquire_result = client.AcquireStory(chronicle_name, story_name, story_attrs, flags);
    assert(acquire_result.first == chronolog::CL_SUCCESS);

    // Replay the story
    std::vector<chronolog::Event> events;
    uint64_t start_time = 1;
    const uint64_t DEFAULT_END_TIME = 2000000000000000000;
    uint64_t end_time = DEFAULT_END_TIME;

    ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, events);
    assert(ret == chronolog::CL_SUCCESS);

    std::cout << "[ReaderExample] Replayed " << events.size() << " events:\n";
    for (const auto& ev : events) {
        std::cout << ev.to_string() << "\n";
    }

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Disconnect from ChronoVisor
    ret = client.Disconnect();
    assert(ret == chronolog::CL_SUCCESS);

    LOG_INFO("[ReaderExample] Finished successfully");
    return 0;
}