#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <cmd_arg_parse.h>
#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono_monitor.h>
#include <common.h>

#define STORY_NAME_LEN 5

struct thread_arg
{
    int tid;
    std::string client_id;
    chronolog::ClientPortalServiceConf portal_conf;
};

void thread_body(struct thread_arg* t)
{
    chronolog::Client client(t->portal_conf);
    int conn_ret = client.Connect();
    if(conn_ret != chronolog::CL_SUCCESS)
    {
        LOG_ERROR("[ClientLibMultiStorytellers] Thread (ID: {}) Failed to connect to ChronoVisor", t->tid);
        return;
    }

    int flags = 0;
    int ret;
    std::string chronicle_name;

    if(t->tid % 2 == 0)
        chronicle_name = "CHRONICLE_2";
    else
        chronicle_name = "CHRONICLE_1";

    std::map<std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    flags = 1;

    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    LOG_DEBUG("[ClientLibMultiStorytellers] Chronicle created: tid={}, ChronicleName={}, Flags: {}",
              t->tid,
              chronicle_name,
              flags);

    std::string story_name = "STORY";
    std::map<std::string, std::string> story_attrs;
    flags = 2;

    auto acquire_ret = client.AcquireStory(chronicle_name, story_name, story_attrs, flags);
    LOG_DEBUG("[ClientLibMultiStorytellers] Story acquired: tid={}, ChronicleName={}, StoryName={}, Ret: {}",
              t->tid,
              chronicle_name,
              story_name,
              chronolog::to_string_client(acquire_ret.first));

    assert(acquire_ret.first == chronolog::CL_SUCCESS || acquire_ret.first == chronolog::CL_ERR_NOT_EXIST ||
           acquire_ret.first == chronolog::CL_ERR_NO_KEEPERS);

    if(chronolog::CL_SUCCESS == acquire_ret.first)
    {
        auto story_handle = acquire_ret.second;
        for(int i = 0; i < 100; ++i)
        {
            story_handle->log_event("line " + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(i % 10));
        }

        ret = client.ReleaseStory(chronicle_name, story_name);
        LOG_DEBUG("[ClientLibMultiStorytellers] Story released: tid={}, ChronicleName={}, StoryName={}, Ret: {}",
                  t->tid,
                  chronicle_name,
                  story_name,
                  chronolog::to_string_client(ret));

        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NO_CONNECTION);
    }

    ret = client.DestroyStory(chronicle_name, story_name);
    LOG_DEBUG("[ClientLibMultiStorytellers] Story destroyed: tid={}, ChronicleName={}, StoryName={}, Ret: {}",
              t->tid,
              chronicle_name,
              story_name,
              chronolog::to_string_client(ret));

    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED ||
           ret == chronolog::CL_ERR_NO_CONNECTION);

    ret = client.DestroyChronicle(chronicle_name);
    LOG_DEBUG("[ClientLibMultiStorytellers] Chronicle destroyed: tid={}, ChronicleName={}", t->tid, chronicle_name);

    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED ||
           ret == chronolog::CL_ERR_NO_CONNECTION);

    client.Disconnect();
}


int main(int argc, char** argv)
{
    int provided;
    std::string client_id = gen_random(8);

    int num_threads = 4;

    std::vector<struct thread_arg> t_args(num_threads);
    std::vector<std::thread> workers(num_threads);

    // Load configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            std::cerr << "[ClientLibMultiStorytellers] Failed to load configuration file '" << conf_file_path
                      << "'. Using default values instead." << std::endl;
        }
        else
        {
            std::cout << "[ClientLibMultiStorytellers] Configuration file loaded successfully from '" << conf_file_path
                      << "'." << std::endl;
        }
    }
    else
    {
        std::cout << "[ClientLibMultiStorytellers] No configuration file provided. Using default values." << std::endl;
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
    if(result == 1)
    {
        return EXIT_FAILURE;
    }

    // Build portal config
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[ClientLibMultiStorytellers] Running test.");

    for(int i = 0; i < num_threads; i++)
    {
        t_args[i].tid = i;
        t_args[i].client_id = client_id;
        t_args[i].portal_conf = portalConf;
        std::thread t{thread_body, &t_args[i]};
        workers[i] = std::move(t);
    }

    for(int i = 0; i < num_threads; i++) workers[i].join();

    return 0;
}
