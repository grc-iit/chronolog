#include <chronolog_client.h>
#include <common.h>
#include <thread>
#include <abt.h>
#include <atomic>
#include <cmd_arg_parse.h>
#include "ClientConfiguration.h"
#include "chrono_monitor.h"

#define CHRONICLE_NAME_LEN 32
#define STORY_NAME_LEN 32

chronolog::Client*client;

struct thread_arg
{
    int tid;
    std::string client_id;
};

void thread_function(void*tt)
{
    struct thread_arg*t = (struct thread_arg*)tt;

    int flags = 0;
    uint64_t offset;
    int ret;
    std::string chronicle_name;
    if(t->tid % 2 == 0) chronicle_name = "Chronicle_1";
    else chronicle_name = "Chronicle_2";
    std::map <std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    chronicle_attrs.emplace("IndexGranularity", "Millisecond");
    chronicle_attrs.emplace("TieringPolicy", "Hot");
    ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
    LOG_DEBUG("[ClientLibMultiArgobotsTest] Thread (ID: {}) - Created Chronicle: {}. Return Code: {}", t->tid
              , chronicle_name, ret);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS ||
           ret == chronolog::CL_ERR_NO_KEEPERS);
    flags = 1;
    std::string story_name = gen_random(STORY_NAME_LEN);
    std::map <std::string, std::string> story_attrs;
    story_attrs.emplace("Priority", "High");
    story_attrs.emplace("IndexGranularity", "Millisecond");
    story_attrs.emplace("TieringPolicy", "Hot");
    flags = 2;
    auto acquire_ret = client->AcquireStory(chronicle_name, story_name, story_attrs, flags);
    LOG_DEBUG(
            "[ClientLibMultiArgobotsTest] Thread ID: {} - Attempted to acquire Story: {} in Chronicle: {}. Result Code: {}"
            , t->tid, story_name, chronicle_name, acquire_ret.first);
    assert(acquire_ret.first == chronolog::CL_SUCCESS || acquire_ret.first == chronolog::CL_ERR_NOT_EXIST ||
           acquire_ret.first == chronolog::CL_ERR_NO_KEEPERS);
    ret = client->DestroyStory(chronicle_name, story_name);

    LOG_DEBUG(
            "[ClientLibMultiArgobotsTest] Thread ID: {} - Attempted to destroy story '{}' within chronicle '{}'. Result Code: {}"
            , t->tid, story_name, chronicle_name, acquire_ret.first);
    assert(ret == chronolog::CL_ERR_ACQUIRED || ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST ||
           ret == chronolog::CL_ERR_NO_KEEPERS);
    ret = client->Disconnect();

    LOG_DEBUG("[ClientLibMultiArgobotsTest] Thread ID: {} - Attempted disconnection. Result Code: {}", t->tid, ret);
    assert(ret == chronolog::CL_ERR_ACQUIRED || ret == chronolog::CL_SUCCESS);
    ret = client->ReleaseStory(chronicle_name, story_name);

    LOG_DEBUG("[ClientLibMultiArgobotsTest] Thread ID: {} - Released Story: {} from Chronicle: {}. Result Code: {}"
              , t->tid, story_name, chronicle_name, ret);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NO_KEEPERS || ret == chronolog::CL_ERR_NOT_EXIST);
    ret = client->DestroyStory(chronicle_name, story_name);

    LOG_DEBUG("[ClientLibMultiArgobotsTest] Thread ID: {} - Destroyed Story: {} from Chronicle: {}. Result Code: {}"
              , t->tid, story_name, chronicle_name, ret);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED ||
           ret == chronolog::CL_ERR_NO_KEEPERS);
    ret = client->DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
}

int main(int argc, char** argv) {
    std::atomic<long> duration_connect{}, duration_disconnect{};
    std::vector<std::thread> thread_vec;

    // Load configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ClientLibMultiArgobotsTest] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ClientLibMultiArgobotsTest] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ClientLibMultiArgobotsTest] No configuration file provided. Using default values." << std::endl;
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

    LOG_INFO("[ClientLibMultiArgobotsTest] Running test...");

    client = new chronolog::Client(portalConf);
    int ret = client->Connect();
    if (ret != chronolog::CL_SUCCESS) {
        LOG_ERROR("[ClientLibMultiArgobotsTest] Failed to connect, ret: {}", ret);
        return EXIT_FAILURE;
    }
    LOG_INFO("[ClientLibMultiArgobotsTest] Connected successfully.");

    int num_xstreams = 8;
    int num_threads = 8;

    ABT_xstream*xstreams = (ABT_xstream*)malloc(sizeof(ABT_xstream) * num_xstreams);
    ABT_pool*pools = (ABT_pool*)malloc(sizeof(ABT_pool) * num_xstreams);
    ABT_thread*threads = (ABT_thread*)malloc(sizeof(ABT_thread) * num_threads);
    std::vector <struct thread_arg> t_args(num_threads);;

    std::string client_id = gen_random(8);;
    std::string server_uri = portalConf.PROTO_CONF + "://" + portalConf.IP + ":" + std::to_string(portalConf.PORT);
    int flags = 0;

    if(ret == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ClientLibMultiArgobotsTest] Connected to the server successfully.");
    }
    else
    {
        LOG_ERROR("[ClientLibMultiArgobotsTest] Failed to connect to the server. Error code: {}", ret);
        exit(1);  // Exit if connection fails
    }
    assert(ret == chronolog::CL_SUCCESS);

    for(int i = 0; i < num_threads; i++)
    {
        t_args[i].tid = i;
        t_args[i].client_id = client_id;
    }

    ABT_init(argc, argv);

    ABT_xstream_self(&xstreams[0]);

    for(int i = 1; i < num_xstreams; i++)
    {
        ABT_xstream_create(ABT_SCHED_NULL, &xstreams[i]);
    }


    for(int i = 0; i < num_xstreams; i++)
    {
        ABT_xstream_get_main_pools(xstreams[i], 1, &pools[i]);
    }


    for(int i = 0; i < num_threads; i++)
    {
        ABT_thread_create(pools[i], thread_function, &t_args[i], ABT_THREAD_ATTR_NULL, &threads[i]);
    }

    for(int i = 0; i < num_threads; i++)
        ABT_thread_free(&threads[i]);

    for(int i = 1; i < num_xstreams; i++)
    {
        ABT_xstream_join(xstreams[i]);
        ABT_xstream_free(&xstreams[i]);
    }

    ABT_finalize();

    free(pools);
    free(xstreams);
    free(threads);

    ret = client->Disconnect(); //client_id, flags);
    LOG_INFO("[ClientLibMultiArgobotsTest] Main disconnect return: {}", ret);
    ret = client->Disconnect();
    if(ret == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ClientLibMultiArgobotsTest] Disconnected from the server successfully.");
    }
    else
    {
        LOG_ERROR("[ClientLibMultiArgobotsTest] Failed to disconnect from the server. Error code: {}", ret);
        // Handle the failure scenario accordingly
    }
    assert(ret == chronolog::CL_SUCCESS);

    delete client;

    return 0;
}