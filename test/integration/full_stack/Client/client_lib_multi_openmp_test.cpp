//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <omp.h>

int main() {

    std::string client_id = gen_random(8);

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    ChronoLogClient *client = new ChronoLogClient(protocol, server_ip, base_port);

    int num_threads = 8;

    omp_set_num_threads(num_threads);

    std::cout <<" omp num_threads = "<<omp_get_num_threads()<<std::endl;

       
    std::vector<std::string> client_ids;

    for(int i=0;i<num_threads;i++)
    {
       std::string id = gen_random(8);
       client_ids.push_back(id);
    }
       
    std::string server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
    server_uri += "://"+server_ip+":"+std::to_string(base_port);
    int flags = 0;
    uint64_t offset;

      #pragma omp for
      for(int i=0;i<num_threads;i++)
      {
       int ret = client->Connect(server_uri,client_ids[i],flags,offset);
       std::cout <<" connected client_id "<<client_ids[i]<<std::endl;
       ret = client->Disconnect(client_ids[i],flags);
       std::cout <<" disconnected"<<std::endl;
      }
    std::cout <<" main thread"<<std::endl;

    delete client;

    return 0;
}
