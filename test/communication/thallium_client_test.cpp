//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
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

int main(int argc, char** argv)
{
    if(argc < 5)
    {
        std::cout << "Usage: " << argv[0]
                  << " <address> <sendrecv|recv|rdma> <msg_size> [repetition]" << std::endl;
        std::cout << "  sendrecv  Round-trip echo via send/recv (server echoes full payload back)" << std::endl;
        std::cout << "  recv      One-way send/recv (server responds with size only)" << std::endl;
        std::cout << "  rdma      One-way RDMA pull (server pulls bulk, responds with size)" << std::endl;
        return 0;
    }
    std::string server_address = argv[1];
    std::string protocol = server_address.substr(0, server_address.find_first_of(':'));
    std::string mode = argv[2];
    long msg_size = strtol(argv[3], nullptr, 10);
    long repetition = 1;
    if(argc > 4)
        repetition = strtol(argv[4], nullptr, 10);
    time_point<my_clock, nanoseconds> t_local_init, t_local_finish;

    chronolog::chrono_monitor::initialize("console",
                                          "thallium_client.log",
                                          chronolog::LogLevel::info,
                                          "thallium_client",
                                          1048576,
                                          3,
                                          chronolog::LogLevel::warn);

    LOG_INFO("[Thallium Client] Configurations:");
    LOG_INFO(" - Server Address: {}", server_address);
    LOG_INFO(" - Protocol: {}", protocol);
    LOG_INFO(" - Mode: {}", mode);
    LOG_INFO(" - Message Size: {} bytes", msg_size);
    LOG_INFO(" - Repetition Count: {}", repetition);

    std::vector<char> data(msg_size, 'Z');

    tl::engine myEngine(protocol, THALLIUM_CLIENT_MODE);

    if(mode == "sendrecv")
    {
        // Round-trip echo: client sends data, server copies and echoes it back
        std::string rpc_name = "repeater";
        LOG_INFO("[Thallium Client] Searching for RPC with name {} on {}", rpc_name, server_address);
        tl::remote_procedure repeater = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        LOG_INFO("[Thallium Client] Initiating send/recv echo mode.");
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++) repeater.on(server)(data);
        t_local_finish = my_clock::now();
    }
    else if(mode == "recv")
    {
        // One-way send/recv: client sends data, server responds with size only
        std::string rpc_name = "receiver";
        LOG_INFO("[Thallium Client] Searching for RPC with name {} on {}", rpc_name, server_address);
        tl::remote_procedure receiver = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        LOG_INFO("[Thallium Client] Initiating one-way recv mode.");
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++) receiver.on(server)(data);
        t_local_finish = my_clock::now();
    }
    else if(mode == "rdma")
    {
        // RDMA: client exposes memory, server pulls it, responds with size
        std::string rpc_name = "rdma_put";
        LOG_INFO("[Thallium Client] Initiating RDMA mode.");
        LOG_INFO("[Thallium Client] Searching for RPC with name {} on {}", rpc_name, server_address);
        tl::remote_procedure rdma_put = myEngine.define(rpc_name);
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&data[0]);
        segments[0].second = data.size();
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        LOG_INFO("[Thallium Client] Sending {} bytes of data to {}", data.size(), server_address);
        t_local_init = my_clock::now();
        for(int i = 0; i < repetition; i++) rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
    }
    else
    {
        LOG_ERROR("[Thallium Client] Invalid mode: {}. Use sendrecv, recv, or rdma.", mode);
        myEngine.finalize();
        return 1;
    }

    myEngine.finalize();

    double total_sec = std::chrono::duration<double>(t_local_finish - t_local_init).count();
    double avg_latency_us = (total_sec / repetition) * 1e6;
    double bandwidth_MBps = ((double)msg_size * repetition) / total_sec / (1024.0 * 1024.0);

    std::cout << std::fixed
              << "mode:        " << mode << std::endl
              << "msg_size:    " << msg_size << std::endl
              << "repetitions: " << repetition << std::endl
              << "total_time:  " << std::setprecision(9) << total_sec << " s" << std::endl
              << "avg_latency: " << std::setprecision(3) << avg_latency_us << " us" << std::endl
              << "bandwidth:   " << std::setprecision(3) << bandwidth_MBps << " MB/s" << std::endl;

    return 0;
}
