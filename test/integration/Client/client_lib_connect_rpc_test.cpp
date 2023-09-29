//
// Created by kfeng on 7/11/22.
//

#include <chrono>
#include <unordered_map>

#include "ConfigurationManager.h"
#include "chronolog_client.h"
#include "common.h"
#include <cassert>
#include <cmd_arg_parse.h>

#define NUM_CONNECTION (1)

int main(int argc, char **argv)
{
    std::string default_conf_file_path = "./default_conf.json";
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if (conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }

    ChronoLog::ConfigurationManager confManager(conf_file_path);
    std::string server_ip = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.IP;
    int base_port = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.BASE_PORT;
    int num_ports = 1; // Kun: hardcode for now
    chronolog::Client client(confManager);
    std::string server_uri;
    std::vector<std::string> client_ids;
    int flags = 0;
    bool ret = false;
    uint64_t offset;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_connect{},
            duration_disconnect{};

    client_ids.reserve(NUM_CONNECTION);
    for (int i = 0; i < NUM_CONNECTION; i++) client_ids.emplace_back(gen_random(8));
    for (int i = 0; i < NUM_CONNECTION; i++)
    {
        switch (confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.RPC_IMPLEMENTATION)
        {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                server_uri = confManager.CLIENT_CONF.VISOR_CLIENT_PORTAL_SERVICE_CONF.RPC_CONF.PROTO_CONF;
                break;
            default:
                server_uri = "ofi+sockets";
        }

        server_uri += "://" + server_ip + ":" + std::to_string(base_port + i);
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect();//server_uri, client_ids[i], flags); //, offset);
        assert(ret == CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for (int i = 0; i < NUM_CONNECTION; i++)
    {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(); //client_ids[i], flags);
        assert(ret == CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1);
    };

    LOGI("Connect takes %lf ns", duration_connect.count() / NUM_CONNECTION);
    LOGI("Disconnect takes %lf ns", duration_disconnect.count() / NUM_CONNECTION);

    return 0;
}
