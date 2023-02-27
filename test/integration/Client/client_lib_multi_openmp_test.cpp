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

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    ChronoLogClient *client = new ChronoLogClient(protocol, server_ip, base_port);

    int num_threads = 8;

    omp_set_num_threads(num_threads);

    std::string server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
    server_uri += "://"+server_ip+":"+std::to_string(base_port);
    int flags = 0;
    uint64_t offset;

   std::string client_id = std::to_string(getpid());
   std::string group_id = "openmp_application";
   client_id = group_id+client_id;

   uint32_t user_role = CHRONOLOG_CLIENT_RWCD;
   uint32_t group_role = CHRONOLOG_CLIENT_GROUP_ADMIN;
   uint32_t cluster_role = CHRONOLOG_CLIENT_CLUS_REG;
   uint32_t role = 0;
   role = role | user_role;
   role = role | (group_role << 3);
   role = role | (cluster_role << 6);
   int ret = client->Connect(server_uri,client_id,group_id,role,flags,offset);
    #pragma omp for
    for(int i=0;i<num_threads;i++)
    {
       std::string chronicle_name;
       if(i%2==0) chronicle_name = "gscs5er9TcdJ9mOgUDteDVBcI0oQjozK";
       else chronicle_name = "6RPkwqX2IOpR41dVCqmWauX9RfXIuTAp";
       std::unordered_map<std::string, std::string> chronicle_attrs;
       chronicle_attrs.emplace("Priority", "High");
       chronicle_attrs.emplace("IndexGranularity", "Millisecond");
       chronicle_attrs.emplace("TieringPolicy", "Hot");
       chronicle_attrs.emplace("Permissions","RWCD");
       ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
       ret = client->AddGrouptoChronicle(chronicle_name,group_id);
       flags = CHRONOLOG_WRITE;
       ret = client->AcquireChronicle(chronicle_name,flags);
       ret = client->ReleaseChronicle(chronicle_name,flags);
       std::string story_name = gen_random(STORY_NAME_LEN);
       std::unordered_map<std::string, std::string> story_attrs;
       story_attrs.emplace("Priority", "High");
       story_attrs.emplace("IndexGranularity", "Millisecond");
       story_attrs.emplace("TieringPolicy", "Hot");
       story_attrs.emplace("Permissions","RWD");
       flags = CHRONOLOG_WRITE;
       ret = client->CreateStory(chronicle_name, story_name, story_attrs, flags);
       ret = client->AcquireStory(chronicle_name,story_name,flags);
       ret = client->ReleaseStory(chronicle_name,story_name,flags);
       ret = client->DestroyStory(chronicle_name,story_name,flags);
       ret = client->DestroyChronicle(chronicle_name,flags);
    }
    ret = client->Disconnect(client_id,flags);

    delete client;

    return 0;
}
