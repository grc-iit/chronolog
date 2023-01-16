//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <omp.h>

#define STORY_NAME_LEN 32

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
       std::string chronicle_name;
       if(i%2==0) chronicle_name = "gscs5er9TcdJ9mOgUDteDVBcI0oQjozK";
       else chronicle_name = "6RPkwqX2IOpR41dVCqmWauX9RfXIuTAp";
       std::unordered_map<std::string, std::string> chronicle_attrs;
       chronicle_attrs.emplace("Priority", "High");
       chronicle_attrs.emplace("IndexGranularity", "Millisecond");
       chronicle_attrs.emplace("TieringPolicy", "Hot");
       ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
       flags = 1;
       ret = client->AcquireChronicle(chronicle_name,flags);
       ret = client->ReleaseChronicle(chronicle_name,flags);
       std::string story_name = gen_random(STORY_NAME_LEN);
       std::unordered_map<std::string, std::string> story_attrs;
       story_attrs.emplace("Priority", "High");
       story_attrs.emplace("IndexGranularity", "Millisecond");
       story_attrs.emplace("TieringPolicy", "Hot");
       flags = 2;
       ret = client->CreateStory(chronicle_name, story_name, story_attrs, flags);
       ret = client->AcquireStory(chronicle_name,story_name,flags);
       ret = client->ReleaseStory(chronicle_name,story_name,flags);
       ret = client->DestroyStory(chronicle_name,story_name,flags);
       ret = client->DestroyChronicle(client_id,flags);
       ret = client->Disconnect(client_ids[i],flags);
      }
    std::cout <<" main thread"<<std::endl;

    delete client;

    return 0;
}
