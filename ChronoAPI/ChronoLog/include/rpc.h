//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_RPC_H
#define CHRONOLOG_RPC_H

#include <thallium.hpp>
#include <margo.h>
#include <thallium/serialization/serialize.hpp>
//#include <thallium/serialization/buffer_input_archive.hpp>
//#include <thallium/serialization/buffer_output_archive.hpp>
#include <thallium/serialization/stl/array.hpp>
#include <thallium/serialization/stl/complex.hpp>
#include <thallium/serialization/stl/deque.hpp>
#include <thallium/serialization/stl/forward_list.hpp>
#include <thallium/serialization/stl/list.hpp>
#include <thallium/serialization/stl/map.hpp>
#include <thallium/serialization/stl/multimap.hpp>
#include <thallium/serialization/stl/multiset.hpp>
#include <thallium/serialization/stl/pair.hpp>
#include <thallium/serialization/stl/set.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/tuple.hpp>
#include <thallium/serialization/stl/unordered_map.hpp>
#include <thallium/serialization/stl/unordered_multimap.hpp>
#include <thallium/serialization/stl/unordered_multiset.hpp>
#include <thallium/serialization/stl/unordered_set.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <future>
#include <data_structures.h>
#include <macro.h>
#include <log.h>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace tl = thallium;

class ChronoLogRPC {
private:
    uint16_t baseServerPort_;
    uint16_t numPorts_;
    uint16_t numStreams_;
    uint16_t clientPort_;
    std::string name;
    bool isRunning_;

    std::vector<tl::engine*> thalliumServerList_;
    std::shared_ptr<tl::engine> thalliumClient_;
    std::vector<ChronoLogCharStruct> serverAddrList_;
    std::vector<tl::endpoint> thallium_endpoints;

    tl::endpoint get_endpoint(ChronoLogCharStruct protocol, ChronoLogCharStruct server_name, uint16_t server_port) {
        // We use addr lookup because mercury addresses must be exactly 15 char
        char ip[16];
        struct hostent *he = gethostbyname(server_name.c_str());
        in_addr **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        ChronoLogCharStruct lookup_str = protocol + "://" + std::string(ip) + ":" + std::to_string(server_port);
        LOGD("lookup_str: %s", lookup_str.c_str());
        return thalliumClient_->lookup(lookup_str.c_str());
    }

    void init_client_engine_and_endpoints(ChronoLogCharStruct protocol) {
        thalliumClient_ = ChronoLog::Singleton<tl::engine>::GetInstance(protocol.c_str(), THALLIUM_CLIENT_MODE,true,numStreams_);
        LOGD("generate a new client at %s", std::string(thalliumClient_->self()).c_str());
        thallium_endpoints.reserve(serverList_.size());
        for (std::vector<ChronoLogCharStruct>::size_type i = 0; i < serverList_.size(); ++i) {
            thallium_endpoints.push_back(get_endpoint(protocol, serverList_[i], baseServerPort_ + i));
        }
    }

    /*std::promise<void> thallium_exit_signal;

      void runThalliumServer(std::future<void> futureObj){

      while(futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout){}
      thallium_engine->wait_for_finalize();
      }*/

    std::vector<ChronoLogCharStruct> serverList_;
public:
    ~ChronoLogRPC() {
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE: {
                    // Mercury addresses in endpoints must be freed before finalizing Thallium
                    thallium_endpoints.clear();
                    for (int i = 0; i < numPorts_; i++) {
                        thalliumServerList_[i]->wait_for_finalize();
                    }
                    thalliumClient_->finalize();
                    break;
                }
            }
        }
    }

    ChronoLogRPC() : baseServerPort_(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT),
            numPorts_(CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS),
            numStreams_(CHRONOLOG_CONF->RPC_NUM_SERVICE_THREADS),
            clientPort_(CHRONOLOG_CONF->RPC_CLIENT_PORT),
            isRunning_(false),
            serverList_() {
        LOGD("ChronoLogRPC constructor is called");
        serverList_ = CHRONOLOG_CONF->LoadServers();
        /* if current rank is a server */
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS: {
                    for (int i = 0; i < numPorts_; i++) {
                        serverAddrList_.emplace_back(CHRONOLOG_CONF->SOCKETS_CONF + "://" +
                                                     CHRONOLOG_CONF->SERVER_LIST[CHRONOLOG_CONF->MY_SERVER_ID] +
                                                     ":" +
                                                     std::to_string(baseServerPort_ + i));
                    }
                    break;
                }
                case CHRONOLOG_THALLIUM_ROCE: {
                    for (int i = 0; i < numPorts_; i++) {
                        serverAddrList_.emplace_back(CHRONOLOG_CONF->VERBS_CONF + "://" +
                                                     CHRONOLOG_CONF->SERVER_LIST[CHRONOLOG_CONF->MY_SERVER_ID] +
                                                     ":" +
                                                     std::to_string(baseServerPort_ + i));
                    }
                    break;
                }
            }
        }
        run(CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS);
    }

    template<typename F>
    void bind(const ChronoLogCharStruct &str, F func);

    void run(size_t workers = CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS) {
        if (CHRONOLOG_CONF->IS_SERVER) {
            /* only servers run */
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE: {
                    for (size_t i = 0; i < workers; i++) 
		    {
			tl::engine *tmpserver = new tl::engine(serverAddrList_[i].c_str(),THALLIUM_SERVER_MODE,true,2);
			thalliumServerList_.push_back(tmpserver);
                    }
		    for(int i=0;i<workers;i++)
			    std::cout <<" server created at "<<thalliumServerList_[i]->self()<<std::endl;
                    break;
                }
            }
        }
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            /* only clients need Thallium end_points */
            if (CHRONOLOG_CONF->IS_SERVER == false) {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS: {
                    init_client_engine_and_endpoints(CHRONOLOG_CONF->SOCKETS_CONF);
                    break;
                }
                case CHRONOLOG_THALLIUM_ROCE: {
                    init_client_engine_and_endpoints(CHRONOLOG_CONF->VERBS_CONF);
                    break;
                }
            }
        }
        isRunning_ = true;
    }

    void start() {
        if (isRunning_) return;
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE: {
                    for (int i = 0; i < numPorts_; i++) {
                        thalliumServerList_[i]->wait_for_finalize();
                    }
//                    thalliumClient_->wait_for_finalize();
                    break;
                }
            }
        }
    }

    template<typename Response, typename... Args>
    Response call(uint16_t server_index,
                  ChronoLogCharStruct const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response call(ChronoLogCharStruct &server,
                  uint16_t &port,
                  ChronoLogCharStruct const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response callWithTimeout(uint16_t server_index,
                             int timeout_ms,
                             ChronoLogCharStruct const &func_name,
                             Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(
            uint16_t server_index,
            ChronoLogCharStruct const &func_name,
            Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(ChronoLogCharStruct &server,
                                     uint16_t &port,
                                     ChronoLogCharStruct const &func_name,
                                     Args... args);

};

#include "rpc.cpp"

#endif //CHRONOLOG_RPC_H
