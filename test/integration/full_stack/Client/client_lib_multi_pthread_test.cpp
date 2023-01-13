//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <cassert>
#include <common.h>
#include <thread>

struct thread_arg
{
  int tid;
};

ChronoLogClient *client;

void thread_body(struct thread_arg *t)
{

    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    std::string client_id;// = gen_random(8);
    std::string server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
    server_uri += "://"+server_ip+":"+std::to_string(base_port);
    int flags = 0;
    uint64_t offset;
    int ret = client->Connect(server_uri,client_id,flags,offset);
    std::cout <<" connected"<<std::endl;
    ret = client->Disconnect(client_id,flags);
    std::cout <<" disconnected"<<std::endl;
}

int main() {

    std::string client_id = gen_random(8);

    int num_threads = 1;

    std::vector<struct thread_arg> t_args(num_threads);
    std::vector<std::thread> workers(num_threads);

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    client = new ChronoLogClient(protocol, server_ip, base_port);

    //thread_body(&t_args[0]);
    for(int i=0;i<num_threads;i++)
    {
	std::thread t{thread_body,&t_args[i]};
	workers[i] = std::move(t);
    }

    for(int i=0;i<num_threads;i++)
	    workers[i].join();

    delete client;

    return 0;
}
