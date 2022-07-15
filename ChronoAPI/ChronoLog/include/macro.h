//
// Created by kfeng on 4/3/22.
//

#ifndef CHRONOLOG_MACRO_H
#define CHRONOLOG_MACRO_H

#include <thallium.hpp>
#include "ConfigurationManager.h"
#include "singleton.h"

#define CHRONOLOG_CONF ChronoLog::Singleton<ChronoLog::ConfigurationManager>::GetInstance()

#define RPC_CALL_WRAPPER_THALLIUM_SOCKETS() case THALLIUM_SOCKETS:
#define RPC_CALL_WRAPPER_THALLIUM_TCP() case THALLIUM_TCP:
#define RPC_CALL_WRAPPER_THALLIUM_ROCE() case THALLIUM_ROCE:

#define RPC_CALL_WRAPPER_THALLIUM(funcname, serverVar, ret, args...) \
{ \
    return rpc->call<tl::packed_data<>>(serverVar, func_prefix + funcname, args).template as<ret>(); \
    break; \
}

#define RPC_CALL_WRAPPER(funcname, serverVar, ret, args...) [& ]()-> ret { \
    switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) { \
        RPC_CALL_WRAPPER_THALLIUM_SOCKETS() \
        RPC_CALL_WRAPPER_THALLIUM_TCP() \
        RPC_CALL_WRAPPER_THALLIUM_ROCE() \
        RPC_CALL_WRAPPER_THALLIUM(funcname, serverVar, ret, args) \
    } \
}();

#define THALLIUM_DEFINE(name, args,args_t...) \
void Thallium##name(const tl::request &thallium_req, args_t) \
{ \
    thallium_req.respond(name args); \
}

#endif //CHRONOLOG_MACRO_H
