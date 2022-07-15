//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_RPC_H
#define CHRONOLOG_RPC_H

#include <thallium.hpp>
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

class RPC {
private:
    uint16_t server_port;
    std::string name;
    bool is_running;

    std::shared_ptr<tl::engine> thallium_server;
    std::shared_ptr<tl::engine> thallium_client;
    CharStruct engine_init_str;
    std::vector<tl::endpoint> thallium_endpoints;

    tl::endpoint get_endpoint(CharStruct protocol, CharStruct server_name, uint16_t server_port) {
        // We use addr lookup because mercury addresses must be exactly 15 char
        char ip[16];
        struct hostent *he = gethostbyname(server_name.c_str());
        in_addr **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        CharStruct lookup_str = protocol + "://" + std::string(ip) + ":" + std::to_string(server_port);
        LOGD("lookup_str: %s", lookup_str.c_str());
        return thallium_client->lookup(lookup_str.c_str());
    }

    void init_engine_and_endpoints(CharStruct protocol) {
        thallium_client = ChronoLog::Singleton<tl::engine>::GetInstance(protocol.c_str(), MARGO_CLIENT_MODE);
        LOGD("generate a new client at %s", std::string(thallium_client->self()).c_str());
        thallium_endpoints.reserve(server_list.size());
        for (std::vector<CharStruct>::size_type i = 0; i < server_list.size(); ++i) {
            thallium_endpoints.push_back(get_endpoint(protocol, server_list[i], server_port + i));
        }
    }

    /*std::promise<void> thallium_exit_signal;

      void runThalliumServer(std::future<void> futureObj){

      while(futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout){}
      thallium_engine->wait_for_finalize();
      }*/

    std::vector<CharStruct> server_list;
public:
    ~RPC() {
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case THALLIUM_TCP:
                case THALLIUM_SOCKETS:
                case THALLIUM_ROCE: {
                    // Mercury addresses in endpoints must be freed before finalizing Thallium
                    thallium_endpoints.clear();
                    thallium_server->finalize();
                    break;
                }
            }
        }
    }

    RPC() : server_list(),
            server_port(CHRONOLOG_CONF->RPC_PORT),
            is_running(false) {
        server_list = CHRONOLOG_CONF->LoadServers();
        /* if current rank is a server */
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case THALLIUM_TCP:
                case THALLIUM_SOCKETS: {
                    engine_init_str = CHRONOLOG_CONF->SOCKETS_CONF + "://" +
                                      CHRONOLOG_CONF->SERVER_LIST[CHRONOLOG_CONF->MY_SERVER_ID] +
                                      ":" +
                                      std::to_string(server_port + CHRONOLOG_CONF->MY_SERVER_ID);
                    break;
                }
                case THALLIUM_ROCE: {
                    engine_init_str = CHRONOLOG_CONF->VERBS_CONF + "://" +
                                      CHRONOLOG_CONF->SERVER_LIST[CHRONOLOG_CONF->MY_SERVER_ID] +
                                      ":" +
                                      std::to_string(server_port + CHRONOLOG_CONF->MY_SERVER_ID);
                    break;
                }
            }
        }
        run(CHRONOLOG_CONF->RPC_THREADS);
    }

    template<typename F>
    void bind(const CharStruct &str, F func);

    void run(size_t workers = CHRONOLOG_CONF->RPC_THREADS) {
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case THALLIUM_TCP:
                case THALLIUM_SOCKETS:
                case THALLIUM_ROCE: {
                    LOGD("running Thallium server with engine str %s", engine_init_str.c_str());
                    thallium_server = ChronoLog::Singleton<tl::engine>::GetInstance(engine_init_str.c_str(),
                                                                                    THALLIUM_SERVER_MODE,
                                                                                    true,
                                                                                    CHRONOLOG_CONF->RPC_THREADS);
                    LOGI("engine: %s", std::string(thallium_server->self()).c_str());
                    break;
                }
            }
        }
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case THALLIUM_TCP:
            case THALLIUM_SOCKETS: {
                init_engine_and_endpoints(CHRONOLOG_CONF->SOCKETS_CONF);
                break;
            }
            case THALLIUM_ROCE: {
                init_engine_and_endpoints(CHRONOLOG_CONF->VERBS_CONF);
                break;
            }
        }
        is_running = true;
    }

    void start() {
        if (is_running) return;
        if (CHRONOLOG_CONF->IS_SERVER) {
            switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
                case THALLIUM_TCP:
                case THALLIUM_SOCKETS:
                case THALLIUM_ROCE: {
                    thallium_server->wait_for_finalize();
                    break;
                }
            }
        }
    }

    template<typename Response, typename... Args>
    Response call(uint16_t server_index,
                  CharStruct const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response call(CharStruct &server,
                  uint16_t &port,
                  CharStruct const &func_name,
                  Args... args);

    template<typename Response, typename... Args>
    Response callWithTimeout(uint16_t server_index,
                             int timeout_ms,
                             CharStruct const &func_name,
                             Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(
            uint16_t server_index,
            CharStruct const &func_name,
            Args... args);

    template<typename Response, typename... Args>
    std::future<Response> async_call(CharStruct &server,
                                     uint16_t &port,
                                     CharStruct const &func_name,
                                     Args... args);

};

#include "rpc.cpp"

#endif //CHRONOLOG_RPC_H
