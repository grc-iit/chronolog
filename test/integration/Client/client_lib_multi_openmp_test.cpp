#include <chronolog_client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <omp.h>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"
#include "client_cmd_arg_parse.h"
#include "ClientConfiguration.h"

#define STORY_NAME_LEN 32

int main(int argc, char**argv)
{
    // Load configuration
    std::string conf_file_path = chronolog::parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ClientLibMultiOpenMPTest] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ClientLibMultiOpenMPTest] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ClientLibMultiOpenMPTest] No configuration file provided. Using default values." << std::endl;
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

    // Build portal config
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[ClientLibMultiOpenMPTest] Running test.");

    std::string server_ip = portalConf.IP;
    int base_port = portalConf.PORT;
    chronolog::Client* client = nullptr;
    client = new chronolog::Client(portalConf);

    int num_threads = 8;

    omp_set_num_threads(num_threads);

    std::string server_uri = portalConf.PROTO_CONF + "://" + portalConf.IP + ":" + std::to_string(portalConf.PORT);
    server_uri += "://" + server_ip + ":" + std::to_string(base_port);
    LOG_INFO("[ClientLibMultiOpenMPTest] Connecting to server at: {}", server_uri);
    int flags = 0;
    uint64_t offset;

    std::string client_id = gen_random(8);
    int ret = client->Connect();//server_uri, client_id, flags);//, offset);
    LOG_INFO("[ClientLibMultiOpenMPTest] Successfully connected to the server.");
#pragma omp for
    for(int i = 0; i < num_threads; i++)
    {
        std::string chronicle_name;
        if(i % 2 == 0) chronicle_name = "gscs5er9TcdJ9mOgUDteDVBcI0oQjozK";
        else chronicle_name = "6RPkwqX2IOpR41dVCqmWauX9RfXIuTAp";
        std::map <std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");

        ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
        LOG_INFO("[ClientLibMultiOpenMPTest] Thread {} creating chronicle: {}", i, chronicle_name);

        flags = 1;
        std::string story_name = gen_random(STORY_NAME_LEN);
        LOG_INFO("[ClientLibMultiOpenMPTest] Thread {} creating story: {}", i, story_name);

        std::map <std::string, std::string> story_attrs;
        story_attrs.emplace("Priority", "High");
        story_attrs.emplace("IndexGranularity", "Millisecond");
        story_attrs.emplace("TieringPolicy", "Hot");
        flags = 2;
        auto acquire_ret = client->AcquireStory(chronicle_name, story_name, story_attrs, flags);

        assert(acquire_ret.first == chronolog::CL_SUCCESS);
        ret = client->DestroyStory(chronicle_name, story_name);//, flags);
        LOG_INFO("[ClientLibMultiOpenMPTest] Thread {} destroying story: {}", i, story_name);

        assert(ret == chronolog::CL_ERR_ACQUIRED);
        ret = client->Disconnect();//client_id, flags);
        assert(ret == chronolog::CL_ERR_ACQUIRED);
        ret = client->ReleaseStory(chronicle_name, story_name);//, flags);
        assert(ret == chronolog::CL_SUCCESS);
        ret = client->DestroyStory(chronicle_name, story_name);//, flags);
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
        ret = client->DestroyChronicle(chronicle_name);//, flags);
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
        LOG_INFO("[ClientLibMultiOpenMPTest] Thread {} destroying chronicle: {}", i, chronicle_name);
    }

    // Disconnecting from the server
    LOG_INFO("[ClientLibMultiOpenMPTest] Disconnecting from the server.");
    ret = client->Disconnect();
    assert(ret == chronolog::CL_SUCCESS);
    LOG_INFO("[ClientLibMultiOpenMPTest] Disconnected successfully.");

    delete client;

    return 0;
}