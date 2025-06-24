#include "ClientConfiguration.h"
#include "client_cmd_arg_parse.h"
#include <chronolog_client.h>
#include "chrono_monitor.h"
#include <common.h>
#include <cassert>

int main(int argc, char** argv) {
    // Load configuration
    std::string conf_file_path = chronolog::parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ClientLibConnectRPCTest] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ClientLibConnectRPCTest] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ClientLibConnectRPCTest] No configuration file provided. Using default values." << std::endl;
    }

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

    // Build portal config
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[ClientExample] Starting ChronoLog Client Example");

    // Create a ChronoLog client
    chronolog::Client client(portalConf);

    // Connect to ChronoVisor
    int ret = client.Connect();
    assert(ret == chronolog::CL_SUCCESS);

    // Create a chronicle
    std::string chronicle_name = "MyChronicle";
    std::map<std::string, std::string> chronicle_attrs;
    int flags = 0;
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    assert(ret == chronolog::CL_SUCCESS);

    // Acquire a story
    std::string story_name = "MyStory";
    std::map<std::string, std::string> story_attrs;
    auto acquire_result = client.AcquireStory(chronicle_name, story_name, story_attrs, flags);
    assert(acquire_result.first == chronolog::CL_SUCCESS);
    auto story_handle = acquire_result.second;

    // Log a few events to the story
    story_handle->log_event("Event 1");
    story_handle->log_event("Event 2");
    story_handle->log_event("Event 3");

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Destroy the story
    ret = client.DestroyStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Destroy the chronicle
    ret = client.DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Disconnect from ChronoVisor
    ret = client.Disconnect();
    assert(ret == chronolog::CL_SUCCESS);

    LOG_INFO("[ClientExample] Finished successfully");
    return 0;
}