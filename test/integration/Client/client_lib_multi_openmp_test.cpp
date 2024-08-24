#include <chronolog_client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <omp.h>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"

#define STORY_NAME_LEN 32

int main(int argc, char**argv)
{
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize(confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGTYPE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGLEVEL
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGNAME
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILESIZE
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.LOGFILENUM
                                                       , confManager.CLIENT_CONF.CLIENT_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[ClientLibMultiOpenMPTest] Running test.");


    std::string server_ip = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP;
    int base_port = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT;
    chronolog::Client*client = new chronolog::Client(confManager);//protocol, server_ip, base_port);

    int num_threads = 8;

    omp_set_num_threads(num_threads);

    std::string server_uri = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF;
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
