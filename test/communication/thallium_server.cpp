//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <thallium.hpp>
#include <array>
#include <string>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/array.hpp>
#include <thread>
#include <algorithm>
#include <unistd.h>
#include <margo.h>
#include "log.h"

#define MAX_BULK_MEM_SIZE (128 * 1024 * 1024)

namespace tl = thallium;

int main(int argc, char**argv)
{
    if(argc != 4)
    {
        std::cerr << "Usage: " << argv[0] << " <address> <nstreams_per_port> <nports>" << std::endl;
        exit(0);
    }

    std::string address = argv[1];
    std::string protocol = address.substr(0, address.find_first_of(':'));
    long nStreams = strtol(argv[2], nullptr, 10);
    long nPorts = strtol(argv[3], nullptr, 10);

    if((protocol.find("sm") != std::string::npos) && nPorts > 1)
    {
        std::cerr << "Multi-port server does not support " << protocol << " protocol, exiting ..." << std::endl;
        exit(1);
    }

    std::vector <std::thread> server_thrd_vec;
    server_thrd_vec.reserve(nPorts);

    tl::abt scope;
    hg_addr_t addr_self;
    hg_return_t hret;

    std::string argobots_conf_str = R"({"enable_profiling" : true,
        "enable_diagnostics" : true,
        "mercury" : {"stats" : true},
        "argobots" : {"abt_mem_max_num_stacks" : 8, "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    std::vector <tl::managed <tl::xstream>> ess;
    Logger::getLogger()->info("Vector of streams created");
    /*std::cout << "vector of streams created" << std::endl;*/
    tl::managed <tl::pool> myPool = tl::pool::create(tl::pool::access::spmc);
    Logger::getLogger()->info("Pool created");
    /*std::cout << "pool created" << std::endl;*/
    for(int j = 0; j < nStreams; j++)
    {
        tl::managed <tl::xstream> es = tl::xstream::create(tl::scheduler::predef::deflt, *myPool);
        Logger::getLogger()->info("A new stream is created");
        /*std::cout << "a new stream is created" << std::endl;*/
        ess.push_back(std::move(es));
    }

    std::vector <tl::engine> engine_vec;
    std::vector <margo_instance_id> mid_vec;
    engine_vec.reserve(nPorts);
    mid_vec.reserve(nPorts);
    for(int j = 0; j < nPorts; j++)
    {
        std::string host_ip = address.substr(0, address.rfind(':'));
        std::string port = address.substr(address.rfind(':') + 1);
        int new_port = stoi(port) + j;
        Logger::getLogger()->info("Newly generated port to use: {}", new_port);
        /*std::cout << "newly generated port to use: " << new_port << std::endl;*/
        std::string new_addr_str = host_ip + ":" + std::to_string(new_port);
        Logger::getLogger()->info("engine no.{}@{}", j, new_addr_str);
        /*std::cout << "engine no." << j << "@" << new_addr_str << std::endl;*/

        margo_instance_id mid = margo_init(new_addr_str.c_str(), MARGO_SERVER_MODE, 0, nStreams);
        if(mid == MARGO_INSTANCE_NULL)
        {
            Logger::getLogger()->error("Error: margo_init()");
            /*std::cerr << "Error: margo_init()" << std::endl;*/
            exit(-1);
        }

        /* figure out first listening addr */
        hret = margo_addr_self(mid, &addr_self);
        if(hret != HG_SUCCESS)
        {
            Logger::getLogger()->error("Error: margo_addr_self()");
            /*std::cerr << "Error: margo_addr_self()" << std::endl;*/
            margo_finalize(mid);
            exit(-1);
        }
        hg_size_t addr_self_string_sz = 128;
        char addr_self_string[128];
        hret = margo_addr_to_string(mid, addr_self_string, &addr_self_string_sz, addr_self);
        if(hret != HG_SUCCESS)
        {
            std::cerr << "Error: margo_addr_to_string()" << std::endl;
            margo_addr_free(mid, addr_self);
            margo_finalize(mid);
            exit(-1);
        }
        margo_addr_free(mid, addr_self);

        tl::engine myEngine(mid);
        Logger::getLogger()->info("Engine created and running@{} ...", std::string(myEngine.self()));
        /*std::cout << "engine created and running@" << std::string(myEngine.self()) << " ..." << std::endl;*/

        // Define send/recv version
        Logger::getLogger()->info("Defining RPC routines in send/recv mode");
        /*std::cout << "Defining RPC routines in send/recv mode" << std::endl;*/
        std::function <void(const tl::request &, std::vector <std::byte> &)> repeater = [](const tl::request &req
                                                                                           , std::vector <std::byte> &data)
        {
            std::vector <std::byte> mem_vec(MAX_BULK_MEM_SIZE);
            std::copy(data.begin(), data.end(), mem_vec.begin());
            Logger::getLogger()->info("Received {} bytes of data in send/recv mode", data.size());
            /*std::cout << "Received " << data.size() << " bytes of data in send/recv mode" << std::endl;*/

        };
        myEngine.define("repeater", repeater, 0, *myPool);

        // Define RDMA version
        Logger::getLogger()->info("Defining RPC routines in RDMA mode");
        /*std::cout << "Defining RPC routines in RDMA mode" << std::endl;*/
        std::function <void(const tl::request &, tl::bulk &)> f = [j, &engine_vec](const tl::request &req, tl::bulk &b)
        {
            Logger::getLogger()->info("RDMA rpc invoked");
            /*std::cout << "RDMA rpc invoked" << std::endl;*/
            tl::endpoint ep = req.get_endpoint();
            Logger::getLogger()->info("endpoint obtained");
            /*std::cout << "endpoint obtained" << std::endl;*/
            std::vector <char> mem_vec(MAX_BULK_MEM_SIZE);
            mem_vec.reserve(MAX_BULK_MEM_SIZE);
            mem_vec.resize(MAX_BULK_MEM_SIZE);
            std::vector <std::pair <void*, std::size_t>> segments(1);
            segments[0].first = (void*)(&mem_vec[0]);
            segments[0].second = mem_vec.size();
            Logger::getLogger()->info("RDMA memory prepared, size: {}", mem_vec.size());
            /*std::cout << "RDMA memory prepared, size: " << mem_vec.size() << std::endl;*/
            tl::engine myEngine = engine_vec.at(j);
            tl::bulk local = myEngine.expose(segments, tl::bulk_mode::write_only);
            Logger::getLogger()->info("RDMA memory exposed");
            /*std::cout << "RDMA memory exposed" << std::endl;*/
            b.on(ep) >> local;
            Logger::getLogger()->info("Received {} bytes of data in RDMA mode", b.size());
            /*std::cout << "Received " << b.size() << " bytes of data in RDMA mode" << std::endl;*/
            //for (auto c : mem_vec) std::cout << c;
            //std::cout << std::endl;
            req.respond();
        };
        myEngine.define("rdma_put", f);//.disable_response();

        engine_vec.emplace_back(std::move(myEngine));
        mid_vec.emplace_back(std::move(mid));
    }
    for(auto engine: engine_vec)
        engine.wait_for_finalize();

    for(int j = 0; j < nStreams; j++)
    {
        ess[j]->join();
    }

    return 0;
}
