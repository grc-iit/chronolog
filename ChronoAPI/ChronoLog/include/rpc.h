//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_RPC_H
#define CHRONOLOG_RPC_H

#include <thallium.hpp>
#include <margo.h>
#include <thallium/serialization/serialize.hpp>
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
#include <macro.h>
#include <log.h>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace tl = thallium;

class ChronoLogRPC
{
private:
    uint16_t baseServerPort_;
    uint16_t numPorts_;
    uint16_t numStreams_;
    uint16_t clientPort_;
    std::string name;
    bool isRunning_;

    std::vector<tl::engine *> thalliumServerList_;
    std::shared_ptr<tl::engine> thalliumClient_;
    std::vector<std::string> serverAddrList_;
    std::vector<std::string> serverRPCAddrList_;
    std::vector<tl::endpoint> thallium_endpoints;

    tl::endpoint get_endpoint(std::string protocol, std::string server_name, uint16_t server_port)
    {
        // We use addr lookup because mercury addresses must be exactly 15 char
        char ip[16];
        struct hostent *he = gethostbyname(server_name.c_str());
        in_addr **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        std::string lookup_str = protocol + "://" + std::string(ip) + ":" + std::to_string(server_port);
        LOGD("lookup_str: %s", lookup_str.c_str());
        return thalliumClient_->lookup(lookup_str.c_str());
    }

    void init_client_engine_and_endpoints(std::string protocol)
    {
        thalliumClient_ = ChronoLog::Singleton<tl::engine>::GetInstance(protocol.c_str(),
                                                                        THALLIUM_CLIENT_MODE, true, numStreams_);
        LOGD("generate a new client at %s", std::string(thalliumClient_->self()).c_str());
        thallium_endpoints.reserve(serverAddrList_.size());
        for (std::vector<std::string>::size_type i = 0; i < serverAddrList_.size(); ++i)
        {
            thallium_endpoints.push_back(get_endpoint(protocol, serverAddrList_[i], baseServerPort_ + i));
        }
    }

    /*std::promise<void> thallium_exit_signal;

      void runThalliumServer(std::future<void> futureObj){

      while(futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout){}
      thallium_engine->wait_for_finalize();
      }*/

public:
    ~ChronoLogRPC()
    {
        if (CHRONOLOG_CONF->ROLE == CHRONOLOG_VISOR)
        {
            switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
            {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE:
                {
                    // Mercury addresses in endpoints must be freed before finalizing Thallium
                    thallium_endpoints.clear();
                    for (int i = 0; i < numPorts_; i++)
                    {
                        thalliumServerList_[i]->wait_for_finalize();
                    }
                    break;
                }
            }
        }
        else thalliumClient_->finalize();
    }

    ChronoLogRPC() : baseServerPort_(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT),
                     numPorts_(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS),
                     numStreams_(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS),
                     clientPort_(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.CLIENT_END_CONF.CLIENT_PORT),
                     isRunning_(false)
    {
        LOGD("ChronoLogRPC constructor is called");
        for (int i = 0; i < numPorts_; i++)
        {
            serverAddrList_.emplace_back(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP);
            serverRPCAddrList_.emplace_back(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF + "://" +
                                            CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP +
                                            ":" +
                                            std::to_string(baseServerPort_ + i));
        }
        run(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS);
    }

    template<typename F>
    void bind(const std::string &str, F func);

    thallium::engine &get_tl_client_engine()
    { return *thalliumClient_; }

    void run(size_t workers = 1)
    {
        if (CHRONOLOG_CONF->ROLE == CHRONOLOG_VISOR)
        {
            /* only servers run */
            switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
            {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE:
                {
                    hg_addr_t addr_self;
                    hg_return_t hret;
                    for (size_t i = 0; i < workers; i++)
                    {
                        LOGD("creating Thallium server with engine str %s", serverRPCAddrList_[i].c_str());
                        for (size_t i = 0; i < workers; i++)  // separate streams and pools for each engine
                        {
                            tl::engine *tmpServer = new tl::engine(serverRPCAddrList_[i].c_str(), THALLIUM_SERVER_MODE,
                                                                   true, numStreams_);
                            thalliumServerList_.push_back(tmpServer);
                        }

                        for (size_t i = 0; i < workers; i++)
                            std::cout << " server created at " << thalliumServerList_[i]->self() << std::endl;
                        break;
                    }
                }
            }
        }
        if (CHRONOLOG_CONF->ROLE == CHRONOLOG_CLIENT)
        {
            switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
            {
                /* only clients need Thallium end_points */
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                {
                    init_client_engine_and_endpoints(CHRONOLOG_CONF->RPC_CONF.AVAIL_PROTO_CONF["sockets_conf"]);
                    break;
                }
                case CHRONOLOG_THALLIUM_ROCE:
                {
                    init_client_engine_and_endpoints(CHRONOLOG_CONF->RPC_CONF.AVAIL_PROTO_CONF["verbs_conf"]);
                    break;
                }
            }
        }
        isRunning_ = true;
    }

    void start()
    {
        if (isRunning_) return;
        if (CHRONOLOG_CONF->ROLE == CHRONOLOG_VISOR)
        {
            switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
            {
                case CHRONOLOG_THALLIUM_TCP:
                case CHRONOLOG_THALLIUM_SOCKETS:
                case CHRONOLOG_THALLIUM_ROCE:
                {
                    for (int i = 0; i < numPorts_; i++)
                    {
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
                  std::string const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response call(std::string &server,
                  uint16_t &port,
                  std::string const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response callWithTimeout(uint16_t server_index,
                             int timeout_ms,
                             std::string const &func_name,
                             Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(
            uint16_t server_index,
            std::string const &func_name,
            Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(std::string &server,
                                     uint16_t &port,
                                     std::string const &func_name,
                                     Args... args);

};

#include "rpc.cpp"

#endif //CHRONOLOG_RPC_H
