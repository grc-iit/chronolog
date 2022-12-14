//
// Created by kfeng on 12/12/22.
//

#ifndef CHRONOLOG_CHRONOLOGADMINRPCCLIENT_H
#define CHRONOLOG_CHRONOLOGADMINRPCCLIENT_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "RPCFactory.h"

class ChronoLogAdminRPCClient {
public:
    ChronoLogAdminRPCClient() {
        LOGD("%s constructor is called", typeid(*this).name());
        rpc = ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()
                ->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT);
        func_prefix = "ChronoLog";
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
    }

    ~ChronoLogAdminRPCClient() = default;

    bool Connect(const std::string &uri, std::string &client_id, int &flags, uint64_t &clock_offset) {
        LOGD("%s in ChronoLogAdminRPCProxy at addresss %p called in PID=%d, with args: uri=%s, client_id=%s",
             __FUNCTION__, this, getpid(), uri.c_str(), client_id.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, bool, uri, client_id, flags, clock_offset);
    }

    bool Disconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("Disconnect", 0, bool, client_id, flags);
    }

private:
    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
};


#endif //CHRONOLOG_CHRONOLOGADMINRPCCLIENT_H
