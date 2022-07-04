//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <chrono>
#include <thallium.hpp>
#include <array>
//#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/vector.hpp>

namespace tl = thallium;

int main(int argc, char** argv) {
    if(argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <address>" << std::endl;
        exit(0);
    }
    std::vector<char> send_vec{};
    send_vec.reserve(10000000);
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
    tl::remote_procedure rdma_put = myEngine.define("rdma_put").disable_response();
    tl::endpoint server = myEngine.lookup(argv[1]);
    std::vector<std::pair<void*,std::size_t>> segments(1);
    segments[0].first  = (void*)(&send_vec[0]);
    segments[0].second = send_vec.size()+1;

    tl::bulk myBulk = myEngine.expose(segments, tl::bulk_mode::read_only);

    using namespace std::chrono;
    using clock = steady_clock;
    time_point<clock, nanoseconds> t1 = clock::now();
    rdma_put.on(server)(myBulk);
    time_point<clock, nanoseconds> t2 = clock::now();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count() << " ns" << std::endl;
    //if (ret_arr == send_arr)
        //std::cout << "Server returns exactly the same array" << std::endl;
    //else
        //std::cout << "Server returns something different" << std::endl;

    return 0;
}