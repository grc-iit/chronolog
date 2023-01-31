//
// Created by kfeng on 7/11/22.
//

#include "client.h"
#include "common.h"
#include <cassert>

#define NUM_CONNECTION (1)

int main() {
    ChronoLogRPCImplementation protocol = CHRONOLOG_THALLIUM_SOCKETS;
    std::string server_ip = "127.0.0.1";
    int base_port = 5555;
    ChronoLogClient client(protocol, server_ip, base_port);
    int num_ports = 3;
    std::string server_uri;
    std::vector<std::string> client_ids;
    int flags = 0;
    bool ret = false;
    int64_t offset;
    double drift_rate;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration<double, std::nano> duration_connect{},
            duration_disconnect{};

    client_ids.reserve(NUM_CONNECTION);
    for (int i = 0; i < NUM_CONNECTION; i++) client_ids.emplace_back(gen_random(8));
    for (int i = 0; i < NUM_CONNECTION; i++) {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                server_uri = CHRONOLOG_CONF->SOCKETS_CONF.string();
                break;
        }
        server_uri += "://" + server_ip + ":" + std::to_string(base_port + i);
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect(server_uri, client_ids[i], flags, offset);
        assert(ret == CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for (int i = 0; i < NUM_CONNECTION; i++) {
        offset = 0;
        drift_rate = 0;
        ret = client.GetClock(offset, drift_rate);
        ASSERT(ret, ==, CL_SUCCESS);
        LOGI("offset: %ld, drift_rate: %f", offset, drift_rate);
    }

    for (int i = 0; i < NUM_CONNECTION; i++) {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(client_ids[i], flags);
        assert(ret == CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1);
    };

    LOGI("CreateChronicle takes %lf ns", duration_connect.count() / NUM_CONNECTION);
    LOGI("AcquireChronicle takes %lf ns", duration_disconnect.count() / NUM_CONNECTION);

    return 0;
}