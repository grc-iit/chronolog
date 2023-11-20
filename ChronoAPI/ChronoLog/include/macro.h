//
// Created by kfeng on 4/3/22.
//

#ifndef CHRONOLOG_MACRO_H
#define CHRONOLOG_MACRO_H

#include <thallium.hpp>
#include "ConfigurationManager.h"
#include "singleton.h"

// it's a bad idea to call GetInstance every time object reference is mentioned ...
#define CHRONOLOG_CONF ChronoLog::Singleton<ChronoLog::ConfigurationManager>::GetInstance()

#define CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_SOCKETS() case CHRONOLOG_THALLIUM_SOCKETS:
#define CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_TCP() case CHRONOLOG_THALLIUM_TCP:
#define CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_ROCE() case CHRONOLOG_THALLIUM_ROCE:

#define CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM(funcname, serverVar, ret, args...) \
{ \
    return rpc->call<ret>(serverVar, func_prefix + funcname, args); \
    break; \
}

#define CHRONOLOG_RPC_CALL_WRAPPER(funcname, serverVar, ret, args...) [& ]()-> ret { \
    switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) { \
        CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_SOCKETS() \
        CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_TCP() \
        CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_ROCE() \
        CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM(funcname, serverVar, ret, args) \
    } \
}();
// this is how it is called: CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, bool, uri, client_id);

#define CHRONOLOG_THALLIUM_DEFINE(name, args, args_t...) \
void Thallium##name(const tl::request &thallium_req, args_t) \
{ \
    thallium_req.respond(name args); \
}

#define ASSERT(left, operator, right) { if(!((left) operator (right))){ \
std::cerr << "ASSERT FAILED: " << #left << #operator << #right \
<< " @ " << __FILE__ << " (" << __LINE__ << "). " \
<< #left << "=" << (left) << "; " << #right << "=" << (right) << std::endl; exit(1); } }

#endif //CHRONOLOG_MACRO_H
