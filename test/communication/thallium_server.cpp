//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <thallium.hpp>
#include <array>
#include <string>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thread>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <margo.h>
#include "log.h"
#include "data_structures.h"

#define MSG_SIZE 10000

namespace tl = thallium;

std::vector<std::string> g_str_vector;

int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <address> <nstreams_per_port> <nports> <sendrecv|rdma>" << std::endl;
        exit(0);
    }

    std::string address = argv[1];
    long nStreams = strtol(argv[2], nullptr, 10);
    long nPorts = strtol(argv[3], nullptr, 10);
    std::string mode = argv[4];
    std::vector<std::thread> server_thrd_vec;
    server_thrd_vec.reserve(nPorts);

    g_str_vector.push_back("100");
    g_str_vector.push_back("200");
    g_str_vector.push_back("300");

    tl::abt scope;
    hg_addr_t addr_self;
    hg_return_t hret;

    std::string argobots_conf_str = R"({"enable_profiling" : true,
        "enable_diagnostics" : true,
        "mercury" : {"stats" : true},
        "argobots" : {"abt_mem_max_num_stacks" : 8, "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    std::vector<tl::managed<tl::xstream>> ess;
    LOGD("vector of streams created");
    tl::managed<tl::pool> myPool = tl::pool::create(tl::pool::access::spmc);
    LOGD("pool created");
    for (int j = 0; j < nStreams; j++) {
        tl::managed<tl::xstream> es
                = tl::xstream::create(tl::scheduler::predef::deflt, *myPool);
        LOGD("a new stream is created");
        ess.push_back(std::move(es));
    }

    std::vector<tl::engine> engine_vec;
    std::vector<margo_instance_id> mid_vec;
    engine_vec.reserve(nPorts);
    mid_vec.reserve(nPorts);
    for (int j = 0; j < nPorts; j++) {
        std::string host_ip = address.substr(0, address.rfind(':'));
        std::string port = address.substr(address.rfind(':') + 1);
        int new_port = stoi(port) + j;
        LOGD("newly generated port to use: %d", new_port);
        std::string new_addr_str = host_ip + ":" + std::to_string(new_port);
        LOGI("engine no.%d@%s", j, new_addr_str.c_str());

        margo_instance_id mid = margo_init(new_addr_str.c_str(), MARGO_SERVER_MODE,
                                           0, nStreams);
        if (mid == MARGO_INSTANCE_NULL) {
            LOGE("Error: margo_init()");
            exit(-1);
        }

        /* figure out first listening addr */
        hret = margo_addr_self(mid, &addr_self);
        if (hret != HG_SUCCESS) {
            LOGE("Error: margo_addr_self()");
            margo_finalize(mid);
            exit(-1);
        }
        hg_size_t addr_self_string_sz = 128;
        char addr_self_string[128];
        hret = margo_addr_to_string(mid, addr_self_string, &addr_self_string_sz,
                                    addr_self);
        if (hret != HG_SUCCESS) {
            LOGE("Error: margo_addr_to_string()");
            margo_addr_free(mid, addr_self);
            margo_finalize(mid);
            exit(-1);
        }
        margo_addr_free(mid, addr_self);

        tl::engine myEngine(mid);
        LOGD("engine created and running@%s ...", std::string(myEngine.self()).c_str());

        if (!strcmp(mode.c_str(), "sendrecv")) {
            // send/recv version
            LOGI("Defining RPC routines in send/recv mode");
            std::function<void(const tl::request &)> return_int =
                    [&j, &engine_vec](const tl::request &req) {
                        req.respond((int) 1234567890);
                    };
            std::function<void(const tl::request &)> return_double =
                    [&j, &engine_vec](const tl::request &req) {
                        req.respond((double) 1234567890.0987654321);
                    };
            std::function<void(const tl::request &)> return_uint64 =
                    [&j, &engine_vec](const tl::request &req) {
                        req.respond((uint64_t) 1234567890.0987654321);
                    };
            std::function<void(const tl::request &)> return_struct =
                    [&j, &engine_vec](const tl::request &req) {
                        GetClockResponse res;
                        res.t_arrival = 1234567890;
                        res.t_departure = 987654321;
                        res.drift_rate = 1234567890.0987654321;
                        req.respond(res);
                    };
            myEngine.define("return_int", return_int, 0, *myPool);
            myEngine.define("return_double", return_double, 0, *myPool);
            myEngine.define("return_uint64", return_uint64, 0, *myPool);
            myEngine.define("return_struct", return_struct, 0, *myPool);
        } else if (!strcmp(mode.c_str(), "rdma")) {
            // RDMA version
            LOGI("Defining RPC routines in RDMA mode");
            std::function<void(const tl::request &, tl::bulk &)> f =
                    [j, &engine_vec](const tl::request &req, tl::bulk &b) {
                        LOGD("RDMA rpc invoked");
                        tl::endpoint ep = req.get_endpoint();
                        LOGD("endpoint obtained");
                        std::vector<char> vec(MSG_SIZE);
                        //vec.reserve(MSG_SIZE);
                        std::vector<std::pair<void *, std::size_t>> segments(1);
                        segments[0].first = (void *) (&vec[0]);
                        segments[0].second = vec.size();
                        LOGD("RDMA memory prepared");
                        tl::engine myEngine = engine_vec.at(j);
                        tl::bulk local = myEngine.expose(segments, tl::bulk_mode::write_only);
                        LOGD("RDMA memory exposed");
                        b.on(ep) >> local;
                    };
            myEngine.define("rdma_put", f).disable_response();
        } else {
            LOGE("Unknonw option %s", mode.c_str());
            exit(-1);
        }
        engine_vec.emplace_back(std::move(myEngine));
        mid_vec.emplace_back(std::move(mid));
    }
    for (auto engine: engine_vec)
        engine.wait_for_finalize();

    for (int j = 0; j < nStreams; j++) {
        ess[j]->join();
    }

    return 0;
}