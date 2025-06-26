#include <atomic>
#include <chronolog_client.h>
#include <common.h>
#include <thread>
#include <atomic>
#include <abt.h>
#include <mpi.h>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"
#include "ClientConfiguration.h"
#include "client_cmd_arg_parse.h"

chronolog::Client*client;

struct thread_arg
{
    int tid;
};

void thread_function(void*t)
{
    LOG_INFO("[ClientLibHybridArgobotsTest] Starting thread function for thread ID: {}", ((thread_arg*)t)->tid);

    chronolog::ClientPortalServiceConf portalConf;
    std::string server_ip = portalConf.IP;
    int base_port = portalConf.PORT;
    std::string client_id = gen_random(8);
    std::string server_uri = portalConf.PROTO_CONF + "://" + portalConf.IP + ":" + std::to_string(portalConf.PORT);
    int flags = 0;

    int ret = client->Connect(); // Connect to server using client_id and flags
    if(ret == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ClientLibHybridArgobotsTest] Successfully connected to server for thread ID: {}"
                 , ((thread_arg*)t)->tid);
    }

    ret = client->Disconnect(); // Disconnect from server using client_id and flags
    if(ret == chronolog::CL_SUCCESS)
    {
        LOG_INFO("[ClientLibHybridArgobotsTest] Successfully disconnected from server for thread ID: {}"
                 , ((thread_arg*)t)->tid);
    }
}

int main(int argc, char**argv)
{
    std::vector <std::string> client_ids;
    std::atomic <long> duration_connect{}, duration_disconnect{};
    std::vector <std::thread> thread_vec;
    uint64_t offset;
    int provided;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    // Load configuration
    std::string conf_file_path = chronolog::parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if (!conf_file_path.empty()) {
        if (!confManager.load_from_file(conf_file_path)) {
            std::cerr << "[ClientLibHybridArgobotsTest] Failed to load configuration file '" << conf_file_path << "'. Using default values instead." << std::endl;
        } else {
            std::cout << "[ClientLibHybridArgobotsTest] Configuration file loaded successfully from '" << conf_file_path << "'." << std::endl;
        }
    } else {
        std::cout << "[ClientLibHybridArgobotsTest] No configuration file provided. Using default values." << std::endl;
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

    LOG_INFO("[ClientLibHybridArgobotsTest] Running test.");

    std::string server_ip = portalConf.IP;
    int base_port = portalConf.PORT;
    client = new chronolog::Client(portalConf);

    int num_xstreams = 8;
    int num_threads = 8;

    ABT_xstream*xstreams = (ABT_xstream*)malloc(sizeof(ABT_xstream) * num_xstreams);
    ABT_pool*pools = (ABT_pool*)malloc(sizeof(ABT_pool) * num_xstreams);
    ABT_thread*threads = (ABT_thread*)malloc(sizeof(ABT_thread) * num_threads);
    struct thread_arg*t_args = (struct thread_arg*)malloc(num_threads * sizeof(struct thread_arg));


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
    free(t_args);

    LOG_INFO("[ClientLibHybridArgobotsTest] Cleaning up resources and finalizing MPI");
    delete client;
    MPI_Finalize();
    LOG_INFO("[ClientLibHybridArgobotsTest] Exiting main function");
    return 0;
}