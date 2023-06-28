
#include <chronolog_client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <chrono>

#define STORY_NAME_LEN 5 

struct thread_arg {
    int tid;
    std::string client_id;
};

chronolog::Client *client;

void thread_body(struct thread_arg *t) {

    int flags = 0;
    uint64_t offset;
    int ret;
    std::string chronicle_name;
    if (t->tid % 2 == 0) chronicle_name = "CHRONICLE_2";
    else chronicle_name = "CHRONICLE_1";
    std::unordered_map<std::string, std::string> chronicle_attrs;
    chronicle_attrs.emplace("Priority", "High");
    flags = 1;
    ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
    std::string story_name = gen_random(STORY_NAME_LEN);
    std::unordered_map<std::string, std::string> story_attrs;
    flags = 2;
    auto acquire_ret = client->AcquireStory(chronicle_name, story_name, story_attrs, flags);
    std::cout << "tid="<<t->tid<<" AcquireStory {"<<chronicle_name<<":"<<story_name<<"} ret: " << acquire_ret.first << std::endl;
    assert(acquire_ret.first == CL_SUCCESS || acquire_ret.first == CL_ERR_NOT_EXIST);

    if(CL_SUCCESS == acquire_ret.first)
    {
        auto story_handle = acquire_ret.second;
        for(int i =0; i< 100; ++i)
        {  
            story_handle->log_event( "line " + std::to_string(i));
            std::this_thread::sleep_for(std::chrono::milliseconds(i%10));
        }
    }
    ret = client->ReleaseStory(chronicle_name, story_name);//, flags);
    std::cout << "tid="<<t->tid<<" ReleaseStory {"<<chronicle_name<<":"<<story_name<<"} ret: " << ret<< std::endl;
    assert(ret == CL_SUCCESS || ret == CL_ERR_NO_CONNECTION);
    ret = client->DestroyStory(chronicle_name, story_name);//, flags);
    std::cout << "tid="<<t->tid<<" DestroyStory {"<<chronicle_name<<":"<<story_name<<"} ret: " << ret<< std::endl;
    assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED || ret ==CL_ERR_NO_CONNECTION);
    ret = client->DestroyChronicle(chronicle_name);//, flags);
    assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED || ret==CL_ERR_NO_CONNECTION);
}

int main(int argc, char **argv) {


    int provided;
    std::string client_id = gen_random(8);

    int num_threads = 4;

    std::vector<struct thread_arg> t_args(num_threads);
    std::vector<std::thread> workers(num_threads);

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    std::string server_ip = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    client = new chronolog::Client(confManager);//protocol, server_ip, base_port);

    std::string server_uri = confManager.RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
    server_uri += "://" + server_ip + ":" + std::to_string(base_port);

    int flags = 0;
    uint64_t offset;
    int ret = client->Connect(server_uri, client_id, flags);//, offset);

    for (int i = 0; i < num_threads; i++) {
        t_args[i].tid = i;
        t_args[i].client_id = client_id;
        std::thread t{thread_body, &t_args[i]};
        workers[i] = std::move(t);
    }

    for (int i = 0; i < num_threads; i++)
        workers[i].join();

    ret = client->Disconnect();//client_id, flags);
    delete client;

    return 0;
}
