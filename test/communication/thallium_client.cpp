//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thallium.hpp>
#include <array>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/array.hpp>
#include "chrono_monitor.h"

namespace tl = thallium;
using namespace std::chrono;
using my_clock = steady_clock;

int main(int argc, char**argv)
{
    if(argc < 5)
    {
        LOG_ERROR("Insufficient arguments provided. Usage: {} <address> <sendrecv|rdma> <msg_size> [repetition]", argv[0]);
        exit(0);
    }
    std::string server_address = argv[1];
    std::string protocol = server_address.substr(0, server_address.find_first_of(':'));
    std::string mode = argv[2];
    long msg_size = strtol(argv[3], nullptr, 10);
    long repetition = 1;
    if(argc > 4)
        repetition = strtol(argv[4], nullptr, 10);
    time_point <my_clock, nanoseconds> t_bigbang, t_local_init, t_local_finish, t_global_finish;

    LOG_INFO("[Thallium Client] Configurations:");
    LOG_INFO(" - Server Address: {}", server_address);
    LOG_INFO(" - Protocol: {}", protocol);
    LOG_INFO(" - Mode: {}", mode);
    LOG_INFO(" - Message Size: {} bytes", msg_size);
    LOG_INFO(" - Repetition Count: {}", repetition);

    std::vector <char> data(msg_size, 'Z');

    t_bigbang = my_clock::now();
    tl::engine myEngine(protocol, THALLIUM_CLIENT_MODE);

    if(!strcmp(mode.c_str(), "sendrecv"))
    {
        /* send/recv version */
        std::string rpc_name = "repeater";
        LOG_INFO("[Thallium Client] Searching for RPC with name {} on {}", rpc_name, server_address);
        tl::remote_procedure repeater = myEngine.define(rpc_name).disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        tl::provider_handle ph(server);
        LOG_INFO("[Thallium Client] Initiating send/recv mode.");
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++)
            repeater.on(server)(data);
        t_local_finish = my_clock::now();
    }
    else if(!strcmp(mode.c_str(), "rdma"))
    {
        /* RDMA version */
        std::string rpc_name = "rdma_put";
        LOG_INFO("[Thallium Client] Initiating RDMA mode.");
        LOG_INFO("[Thallium Client] Searching for RPC with name {} on {}", rpc_name, server_address);
        tl::remote_procedure rdma_put = myEngine.define(rpc_name);//.disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&data[0]);
        segments[0].second = data.size() + 1;
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        LOG_INFO("[Thallium Client] Sending {} bytes of data to {}", data.size(), server_address);
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++)
            rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
    }
    else
    {
        LOG_ERROR("[Thallium Client] Invalid mode selected: {}", mode);
        exit(0);
    }

    myEngine.finalize();
    // sleep(100);

    double duration = std::chrono::duration <double>(t_local_finish - t_bigbang).count();
    LOG_INFO("[Thallium Client] Total execution time: {:>10} seconds", duration);

    return 0;
}
