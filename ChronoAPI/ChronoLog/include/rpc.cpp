//
// Created by kfeng on 3/30/22.
//

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

template <typename F>
void ChronoLogRPC::bind(const std::string &str, F func)
{
    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        case CHRONOLOG_THALLIUM_SOCKETS:
        case CHRONOLOG_THALLIUM_ROCE:
        {
            for(int i = 0; i < numPorts_; i++)
                thalliumServerList_[i]->define(str.string(), func);
//            thalliumClient_->define(str.string(), func);
            break;
        }
    }
}

template <typename Response, typename... Args>
Response
ChronoLogRPC::callWithTimeout(uint16_t server_index, int timeout_ms, std::string const &func_name, Args... args)
{
    int16_t port = baseServerPort_ + server_index;

    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        case CHRONOLOG_THALLIUM_SOCKETS:
        case CHRONOLOG_THALLIUM_ROCE:
        {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            return remote_procedure.on(thallium_endpoints[server_index])(std::forward <Args>(args)...);
            break;
        }
    }
}

template <typename Response, typename... Args>
Response ChronoLogRPC::call(uint16_t server_index, std::string const &func_name, Args... args)
{
//    int16_t port = baseServerPort_ + server_index;

    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        case CHRONOLOG_THALLIUM_SOCKETS:
        case CHRONOLOG_THALLIUM_ROCE:
        {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            return remote_procedure.on(thallium_endpoints[server_index])(std::forward <Args>(args)...);
            break;
        }
    }
}

template <typename Response, typename... Args>
Response ChronoLogRPC::call(std::string &server, uint16_t &port, std::string const &func_name, Args... args)
{
    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        case CHRONOLOG_THALLIUM_SOCKETS:
        case CHRONOLOG_THALLIUM_ROCE:
        {
            tl::remote_procedure remote_procedure = thalliumClient_->define(func_name.c_str());
            auto end_point = get_endpoint(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF, server, port);
            return remote_procedure.on(end_point)(std::forward <Args>(args)...);
            break;
        }
    }
}

template <typename Response, typename... Args>
std::future <Response> ChronoLogRPC::async_call(uint16_t server_index, std::string const &func_name, Args... args)
{
//    int16_t port = baseServerPort_ + server_index;

    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        {
            // TODO: NotImplemented error
            Logger::getLogger()->error("[RPC] Async call with TCP protocol is not supported yet.");
            break;
        }
        case CHRONOLOG_THALLIUM_SOCKETS:
        {
            // TODO: NotImplemented error
            Logger::getLogger()->error("[RPC] Async call with SOCKETS protocol is not supported yet.");
            break;
        }
        case CHRONOLOG_THALLIUM_ROCE:
        {
            // TODO: NotImplemented error
            Logger::getLogger()->error("[RPC] Async call with ROCE protocol is not supported yet.");
            break;
        }
    }
}

template <typename Response, typename... Args>
std::future <Response>
ChronoLogRPC::async_call(std::string &server, uint16_t &port, std::string const &func_name, Args... args)
{
    switch(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION)
    {
        case CHRONOLOG_THALLIUM_TCP:
        {
            // TODO: NotImplemented error
            break;
        }
        case CHRONOLOG_THALLIUM_SOCKETS:
        {
            // TODO: NotImplemented error
        }
        case CHRONOLOG_THALLIUM_ROCE:
        {
            // TODO: NotImplemented error
            break;
        }
    }
}
