//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <chrono>
#include <thallium.hpp>
#include <array>
//#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/vector.hpp>

#define REPS 10000

namespace tl = thallium;

int main(int argc, char** argv) {
    if(argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <address>" << std::endl;
        exit(0);
    }
    std::vector<char> send_vec{};
    send_vec.reserve(10000000);
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
//    tl::remote_procedure rdma_put = myEngine.define("rdma_put").disable_response();
//    tl::endpoint server = myEngine.lookup(argv[1]);
//    std::vector<std::pair<void*,std::size_t>> segments(1);
//    segments[0].first  = (void*)(&send_vec[0]);
//    segments[0].second = send_vec.size()+1;
//
//    tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);
//
//    using namespace std::chrono;
//    using clock = steady_clock;
//    time_point<clock, nanoseconds> t1 = clock::now();
//    rdma_put.on(server)(myBulk);
//    time_point<clock, nanoseconds> t2 = clock::now();
//    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count() << " ns" << std::endl;
//    //if (ret_arr == send_arr)
//        //std::cout << "Server returns exactly the same array" << std::endl;
//    //else
//        //std::cout << "Server returns something different" << std::endl;
    std::vector<std::string> rpc_names = {"return_int", "return_double", "return_uint64", "return_struct"};
    std::unordered_map<std::string, std::chrono::nanoseconds> duration_vec;
    for (auto &name : rpc_names)
        duration_vec.insert_or_assign(name, std::chrono::nanoseconds(0));
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds> t1, t2;
    for (int i = 0; i < REPS; i++) {
        for (auto &name: rpc_names) {
            tl::remote_procedure rp = myEngine.define(name);
            tl::endpoint server = myEngine.lookup(argv[1]);
            t1 = std::chrono::steady_clock::now();
            auto res = rp.on(server)();
            t2 = std::chrono::steady_clock::now();
            auto duration = duration_vec[name];
            duration += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1);
            duration_vec.insert_or_assign(name, duration);

        }
    }
    for (auto [name, duration] : duration_vec) {
        std::cout << "Server " << name << " returns in " << duration.count() / REPS << " ns" << std::endl;
    }
    return 0;
}