#include <chronolog_client.h>
#include <cassert>
#include <common.h>
#include <thread>
#include <omp.h>

#define STORY_NAME_LEN 32

int main() {

    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    std::string server_ip = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    chronolog::Client *client = new chronolog::Client(confManager);//protocol, server_ip, base_port);

    int num_threads = 8;

    omp_set_num_threads(num_threads);

    std::string server_uri = confManager.RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
    server_uri += "://" + server_ip + ":" + std::to_string(base_port);
    int flags = 0;
    uint64_t offset;

    std::string client_id = gen_random(8);
    int ret = client->Connect(server_uri, client_id, flags);//, offset);
#pragma omp for
    for (int i = 0; i < num_threads; i++) {
        std::string chronicle_name;
        if (i % 2 == 0) chronicle_name = "gscs5er9TcdJ9mOgUDteDVBcI0oQjozK";
        else chronicle_name = "6RPkwqX2IOpR41dVCqmWauX9RfXIuTAp";
        std::unordered_map<std::string, std::string> chronicle_attrs;
        chronicle_attrs.emplace("Priority", "High");
        chronicle_attrs.emplace("IndexGranularity", "Millisecond");
        chronicle_attrs.emplace("TieringPolicy", "Hot");
        ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
        flags = 1;
        std::string story_name = gen_random(STORY_NAME_LEN);
        std::unordered_map<std::string, std::string> story_attrs;
        story_attrs.emplace("Priority", "High");
        story_attrs.emplace("IndexGranularity", "Millisecond");
        story_attrs.emplace("TieringPolicy", "Hot");
        flags = 2;
        auto acquire_ret = client->AcquireStory(chronicle_name, story_name, story_attrs, flags);
        assert(acquire_ret.first == CL_SUCCESS);
        ret = client->DestroyStory(chronicle_name, story_name);//, flags);
        assert(ret == CL_ERR_ACQUIRED);
        ret = client->Disconnect();//client_id, flags);
        assert(ret == CL_ERR_ACQUIRED);
        ret = client->ReleaseStory(chronicle_name, story_name);//, flags);
        assert(ret == CL_SUCCESS);
        ret = client->DestroyStory(chronicle_name, story_name);//, flags);
        assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED);
        ret = client->DestroyChronicle(chronicle_name);//, flags);
        assert(ret == CL_SUCCESS || ret == CL_ERR_NOT_EXIST || ret == CL_ERR_ACQUIRED);
    }
    ret = client->Disconnect();//client_id, flags);

    delete client;

    return 0;
}
