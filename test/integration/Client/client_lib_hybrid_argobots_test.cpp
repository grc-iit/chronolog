#include <chronolog_client.h>
#include <common.h>
#include <thread>
#include <abt.h>
#include <mpi.h>

chronolog::Client *client;

struct thread_arg {
    int tid;
};

void thread_function(void *t) {

    auto CHRONOLOG_CONF = ChronoLog::Singleton<ChronoLog::ConfigurationManager>::GetInstance()
  
    std::string server_ip = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    std::string client_id = gen_random(8);
    std::string server_uri = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
    server_uri += "://" + server_ip + ":" + std::to_string(base_port);
    int flags = 0;
    uint64_t offset;
    int ret = client->Connect(server_uri, client_id, flags);//, offset);
    ret = client->Disconnect();//client_id, flags);
}

int main(int argc, char **argv) {
    std::vector<std::string> client_ids;
    std::atomic<long> duration_connect{}, duration_disconnect{};
    std::vector<std::thread> thread_vec;
    uint64_t offset;
    int provided;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    ChronoLog::ConfigurationManager confManager;

    std::string server_ip = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    client = new chronolog::Client(confManager); // protocol, server_ip, base_port);

    int num_xstreams = 8;
    int num_threads = 8;

    ABT_xstream *xstreams = (ABT_xstream *) malloc(sizeof(ABT_xstream) * num_xstreams);
    ABT_pool *pools = (ABT_pool *) malloc(sizeof(ABT_pool) * num_xstreams);
    ABT_thread *threads = (ABT_thread *) malloc(sizeof(ABT_thread) * num_threads);
    struct thread_arg *t_args = (struct thread_arg *) malloc(num_threads * sizeof(struct thread_arg));


    ABT_init(argc, argv);

    ABT_xstream_self(&xstreams[0]);

    for (int i = 1; i < num_xstreams; i++) {
        ABT_xstream_create(ABT_SCHED_NULL, &xstreams[i]);
    }


    for (int i = 0; i < num_xstreams; i++) {
        ABT_xstream_get_main_pools(xstreams[i], 1, &pools[i]);
    }


    for (int i = 0; i < num_threads; i++) {
        ABT_thread_create(pools[i], thread_function, &t_args[i], ABT_THREAD_ATTR_NULL, &threads[i]);
    }

    for (int i = 0; i < num_threads; i++)
        ABT_thread_free(&threads[i]);

    for (int i = 1; i < num_xstreams; i++) {
        ABT_xstream_join(xstreams[i]);
        ABT_xstream_free(&xstreams[i]);
    }

    ABT_finalize();

    free(pools);
    free(xstreams);
    free(threads);
    free(t_args);

    delete client;

    MPI_Finalize();

    return 0;
}
