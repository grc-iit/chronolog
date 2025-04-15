//
// Created by kfeng on 7/11/22.
//

#include <chrono>
#include <unordered_map>
#include "chronolog_client.h"
#include "common.h"
#include <cassert>
#include <cmd_arg_parse.h>
#include "chrono_monitor.h"

#define NUM_CONNECTION (1)

int main(int argc, char**argv)
{
    chronolog::ClientPortalServiceConf portalConf("ofi+sockets", "127.0.0.1", 5555, 55);
    int result = chronolog::chrono_monitor::initialize("file", "chronoclient_logfile.txt", spdlog::level::debug, "ChronoClient", 102400, 3, spdlog::level::warn);
    
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[ClientLibConnectRPCTest] Running test.");

    std::string server_ip = portalConf.ip();
    int base_port = portalConf.port();
    int num_ports = 1; // Kun: hardcode for now
    chronolog::Client client(portalConf);
    std::string server_uri;
    std::vector <std::string> client_ids;
    int flags = 0;
    bool ret = false;
    uint64_t offset;
    std::chrono::steady_clock::time_point t1, t2;
    std::chrono::duration <double, std::nano> duration_connect{}, duration_disconnect{};

    client_ids.reserve(NUM_CONNECTION);
    for(int i = 0; i < NUM_CONNECTION; i++) client_ids.emplace_back(gen_random(8));
    for(int i = 0; i < NUM_CONNECTION; i++)
    {
        server_uri = portalConf.proto_conf() + "://" + server_ip + ":" + std::to_string(base_port + i);
         + "://" + server_ip + ":" + std::to_string(base_port + i);
        t1 = std::chrono::steady_clock::now();
        ret = client.Connect();//server_uri, client_ids[i], flags); //, offset);
        assert(ret == chronolog::CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_connect += (t2 - t1);
    }

    for(int i = 0; i < NUM_CONNECTION; i++)
    {
        t1 = std::chrono::steady_clock::now();
        ret = client.Disconnect(); //client_ids[i], flags);
        assert(ret == chronolog::CL_SUCCESS);
        t2 = std::chrono::steady_clock::now();
        duration_disconnect += (t2 - t1);
    };

    LOG_INFO("[ClientLibConnectRPCTest] Average connection time: {} ns", duration_connect.count() / NUM_CONNECTION);
    LOG_INFO("[ClientLibConnectRPCTest] Average disconnection time: {} ns",
            duration_disconnect.count() / NUM_CONNECTION);

    return 0;
}
