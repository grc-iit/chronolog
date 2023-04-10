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

namespace tl = thallium;
using namespace std::chrono;
using my_clock = steady_clock;

int main(int argc, char** argv) {
    if(argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <address> <sendrecv|rdma> <msg_size> [repetition]" << std::endl;
        exit(0);
    }
    std::string server_address = argv[1];
    std::string protocol = server_address.substr(0, server_address.find_first_of(':'));
    std::string mode = argv[2];
    long msg_size = strtol(argv[3], nullptr, 10);
    long repetition = 1;
    if (argc > 4)
        repetition = strtol(argv[4], nullptr, 10);
    time_point<my_clock, nanoseconds> t_bigbang, t_local_init,
            t_local_finish, t_global_finish;

    std::cout << "server_address: " << server_address << std::endl;
    std::cout << "protocol: " << protocol << std::endl;
    std::cout << "mode: " << mode << std::endl;
    std::cout << "msg_size: " << msg_size << std::endl;
    std::cout << "repetition: " << repetition << std::endl;

    std::vector<char> data(msg_size, 'Z');

    t_bigbang = my_clock::now();
    tl::engine myEngine(protocol, THALLIUM_CLIENT_MODE);

    if (!strcmp(mode.c_str(), "sendrecv")) {
        /* send/recv version */
        std::string rpc_name = "repeater";
        std::cout << "Searching for RPC with name " << rpc_name << " on " << server_address << std::endl;
        tl::remote_procedure repeater = myEngine.define(rpc_name).disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        tl::provider_handle ph(server);
        std::cout << "RPC found, calling ..." << std::endl;
        t_local_init = my_clock::now();
        for (int i = 0; i < repetition; i++)
            repeater.on(server)(data);
        t_local_finish = my_clock::now();
    } else if (!strcmp(mode.c_str(), "rdma")) {
        /* RDMA version */
        std::string rpc_name = "rdma_put";
        std::cout << "Searching for RPC with name " << rpc_name << " on " << server_address << std::endl;
        tl::remote_procedure rdma_put = myEngine.define(rpc_name);//.disable_response();
        tl::endpoint server = myEngine.lookup(server_address);
        std::vector<std::pair<void *, std::size_t>> segments(1);
        segments[0].first = (void *) (&data[0]);
        segments[0].second = data.size() + 1;
        tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
        std::cout << "Sending " << data.size() << " bytes of data to " << server_address << std::endl;
        //for (auto c : data) std::cout << c;
        //std::cout << std::endl;
        t_local_init = my_clock::now();
        for (int i = 0; i < repetition; i++)
            rdma_put.on(server)(myBulk);
        t_local_finish = my_clock::now();
    } else {
        std::cout << "Unknown option " << mode << std::endl;
        exit(0);
    }

    myEngine.finalize();
//    sleep(100);

    double duration = std::chrono::duration<double>(t_local_finish - t_bigbang).count();
    std::cout << "Time used: "
              << std::setw(10) << duration << " second"
              << std::endl;
    //if (ret_arr == send_arr)
        //std::cout << "Server returns exactly the same array" << std::endl;
    //else
        //std::cout << "Server returns something different" << std::endl;

    return 0;
}
