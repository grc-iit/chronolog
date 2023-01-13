//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <common.h>
#include <thread>
#include <abt.h>

ChronoLogClient *client;

struct thread_arg
{
  int tid;

};

void thread_function(void *t)
{

	std::string server_ip = "127.0.0.1";
	int base_port = 5555;
	std::string client_id = gen_random(8);
	std::string server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
	server_uri += "://"+server_ip+":"+std::to_string(base_port);
        int flags = 0;
	uint64_t offset;
	int ret = client->Connect(server_uri,client_id,flags,offset);
	ret = client->Disconnect(client_id,flags);
}

int main(int argc,char **argv) {
    std::vector<std::string> client_ids;
    std::atomic<long> duration_connect{}, duration_disconnect{};
    std::vector<std::thread> thread_vec;
    uint64_t offset;


    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    client = new ChronoLogClient(protocol, server_ip, base_port);

    int num_xstreams = 8;
    int num_threads = 8;

    ABT_xstream *xstreams = (ABT_xstream *)malloc(sizeof(ABT_xstream) * num_xstreams);
    ABT_pool *pools = (ABT_pool *)malloc(sizeof(ABT_pool) * num_xstreams);
    ABT_thread *threads = (ABT_thread*)malloc(sizeof(ABT_thread)*num_threads);
    struct thread_arg *t_args = (struct thread_arg*)malloc(num_threads*sizeof(struct thread_arg));


    ABT_init(argc, argv);

    ABT_xstream_self(&xstreams[0]);

    for (int i = 1; i < num_xstreams; i++) 
    {
        ABT_xstream_create(ABT_SCHED_NULL, &xstreams[i]);
    }


    for (int i = 0; i < num_xstreams; i++) 
    {
        ABT_xstream_get_main_pools(xstreams[i], 1, &pools[i]);
    }


   for(int i=0;i<num_threads;i++)
   {
       ABT_thread_create(pools[i],thread_function,&t_args[i],ABT_THREAD_ATTR_NULL, &threads[i]);
   }

   for(int i=0;i<num_threads;i++)
   ABT_thread_free(&threads[i]);

   for (int i = 1; i < num_xstreams; i++) 
   {
        ABT_xstream_join(xstreams[i]);
        ABT_xstream_free(&xstreams[i]);
   }

   ABT_finalize();

   free(pools);
   free(xstreams);
   free(threads);
   free(t_args);

    delete client;

    return 0;
}
