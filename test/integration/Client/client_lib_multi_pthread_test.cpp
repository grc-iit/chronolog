//
// Created by kfeng on 7/18/22.
//
#include <client.h>
#include <cassert>
#include <common.h>
#include <thread>
#define STORY_NAME_LEN 32

struct thread_arg
{
  int tid;
};

ChronoLogClient *client;

void thread_body(struct thread_arg *t)
{

    std::string server_ip = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    int flags = 0;
    uint64_t offset;
    int ret;
    std::string chronicle_name;
    if(t->tid%2==0) chronicle_name = "gscs5er9TcdJ9mOgUDteDVBcI0oQjozK";
    else chronicle_name = "6RPkwqX2IOpR41dVCqmWauX9RfXIuTAp";
    std::unordered_map<std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    chronicle_attrs.emplace("IndexGranularity", "Millisecond");
    chronicle_attrs.emplace("TieringPolicy", "Hot");
    chronicle_attrs.emplace("Permissions","RWCD");
    ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
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

int main(int argc,char **argv) {


    int provided;
    std::string client_id = std::to_string(getpid());
    std::string group_id = "pthreads_application";
    client_id = group_id+client_id;
    int num_threads = 4;

    std::vector<struct thread_arg> t_args(num_threads);
    std::vector<std::thread> workers(num_threads);

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    client = new ChronoLogClient(protocol, server_ip, base_port);

    std::string server_uri = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
    server_uri += "://"+server_ip+":"+std::to_string(base_port);

    uint32_t role = CHRONOLOG_CLIENT_REGULAR_USER;
    int flags = 0;
    uint64_t offset;
    int ret = client->Connect(server_uri,client_id,group_id,role,flags,offset);

    for(int i=0;i<num_threads;i++)
    {
	t_args[i].tid = i;
	std::thread t{thread_body,&t_args[i]};
	workers[i] = std::move(t);
    }

    for(int i=0;i<num_threads;i++)
	    workers[i].join();

    ret = client->Disconnect(client_id,flags);
    delete client;

    return 0;
}
