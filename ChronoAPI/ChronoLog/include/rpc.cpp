//
// Created by kfeng on 3/30/22.
//

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

template<typename F>
void RPC::bind(const CharStruct &str, F func) {
    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP:
        case THALLIUM_SOCKETS:
        case THALLIUM_ROCE: {
            for (int i = 0; i < numPorts_; i++)
                thalliumServerList_[i].define(str.string(), func);
//            thalliumClient_->define(str.string(), func);
            break;
        }
    }
}

template<typename Response, typename... Args>
Response RPC::callWithTimeout(uint16_t server_index, int timeout_ms, CharStruct const &func_name, Args... args) {
    int16_t port = baseServerPort_ + server_index;

    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP:
        case THALLIUM_SOCKETS:
        case THALLIUM_ROCE: {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            return remote_procedure.on(thallium_endpoints[server_index])(std::forward<Args>(args)...);
            break;
        }
    }
}

template<typename Response, typename... Args>
Response RPC::call(uint16_t server_index,
                   CharStruct const &func_name,
                   Args... args) {
//    int16_t port = baseServerPort_ + server_index;

    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP:
        case THALLIUM_SOCKETS:
        case THALLIUM_ROCE: {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            return remote_procedure.on(thallium_endpoints[server_index])(std::forward<Args>(args)...);
            break;
        }
    }
}

template<typename Response, typename... Args>
Response RPC::call(CharStruct &server,
                   uint16_t &port,
                   CharStruct const &func_name,
                   Args... args) {
    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP:
        case THALLIUM_SOCKETS:
        case THALLIUM_ROCE: {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            auto end_point = get_endpoint(CHRONOLOG_CONF->SOCKETS_CONF, server, port);
            return remote_procedure.on(end_point)(std::forward<Args>(args)...);
            break;
        }
    }
}

template<typename Response, typename... Args>
std::future<Response> RPC::async_call(uint16_t server_index,
                                      CharStruct const &func_name,
                                      Args... args) {
//    int16_t port = baseServerPort_ + server_index;

    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP: {
            // TODO: NotImplemented error
            std::cout << "async call with protocol TCP IS NOT SUPPORTED YET" << std::endl;
            break;
        }
        case THALLIUM_SOCKETS: {
            // TODO: NotImplemented error
            std::cout << "async call with protocol SOCKETS IS NOT SUPPORTED YET" << std::endl;
            break;
        }
        case THALLIUM_ROCE: {
            // TODO: NotImplemented error
            std::cout << "async call with protocol ROCE IS NOT SUPPORTED YET" << std::endl;
            break;
        }
    }
}

template<typename Response, typename... Args>
std::future<Response> RPC::async_call(CharStruct &server,
                                      uint16_t &port,
                                      CharStruct const &func_name,
                                      Args... args) {
    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
        case THALLIUM_TCP: {
            // TODO: NotImplemented error
            break;
        }
        case THALLIUM_SOCKETS: {
            // TODO: NotImplemented error
        }
        case THALLIUM_ROCE: {
            // TODO: NotImplemented error
            break;
        }
    }
}