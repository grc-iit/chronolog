//
// Created by kfeng on 7/11/22.
//

#include <unordered_map>

#include "ConfigurationManager.h"
#include "chronolog_client.h"
#include "common.h"
#include <cassert>

#define NUM_CONNECTION (1)

int main() {
    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    ChronoLog::ConfigurationManager confManager("./default_conf.json");
    std::string server_ip = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
    int base_port = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
    int num_ports = confManager.RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS;
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
    for (int i = 0; i < NUM_CONNECTION; i++) {
        switch (confManager.RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                server_uri = confManager.RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
                break;
	    default:
		server_uri="ofi+sockets";
        }

        server_uri += "://" + server_ip + ":" + std::to_string(base_port + i);
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect(server_uri, client_ids[i], flags); //, offset);
        assert(ret == CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for (int i = 0; i < NUM_CONNECTION; i++) {
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
