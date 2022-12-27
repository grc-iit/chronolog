//
// Created by kfeng on 3/26/22.
//

#include <iostream>
#include <thallium.hpp>
#include <array>
#include <string>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <../../../ChronoVisor/include/ClientRegistryManager.h>
#include <../../ChronoLog/include/singleton.h>
#include <thread>
#include <string>
#include <algorithm>
#include <unistd.h>
#include <margo.h>
#include "log.h"
#include "global_var_visor.h"

#define MSG_SIZE 100

namespace tl = thallium;

std::vector<std::string> g_str_vector;

using namespace ChronoLog;
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

    std::shared_ptr<ClientRegistryManager> g_clientRegistryManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
    ClientRegistryInfo record;
    record.addr_ = "127.0.0.1";
    g_clientRegistryManager->add_client_record("1000000", record);
    g_clientRegistryManager->add_client_record("2000000", record);
    g_clientRegistryManager->add_client_record("3000000", record);

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
            std::function<void(const tl::request &, std::vector<char> &)> repeater =
                    [&j, &engine_vec, &g_clientRegistryManager](const tl::request &req, std::vector<char> &data) {
                        std::cout << "global vector has " << g_str_vector.size() << " elements" << std::endl;
                        std::cout << "global vector[1]: " << g_str_vector[1] << std::endl;
                        int flag = 0;
                        g_clientRegistryManager->remove_client_record("1000000", flag);
                        req.respond(data);
                    };
            myEngine.define("repeater", repeater, 0, *myPool);
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