//
// Created by kfeng on 7/4/22.
//

#ifndef CHRONOLOG_CHRONOLOGADMINRPCPROXY_H
#define CHRONOLOG_CHRONOLOGADMINRPCPROXY_H

#include <iostream>
#include <unordered_map>
#include <functional>
#include <thallium.hpp>
#include <sys/types.h>
#include <unistd.h>
#include "macro.h"
#include "RPCFactory.h"
#include "ClientRegistryInfo.h"
#include "ClientRegistryManager.h"

class ChronoLogAdminRPCProxy {
public:
    ChronoLogAdminRPCProxy() {
        LOGD("%s constructor is called", typeid(*this).name());
        rpc = ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT);
        func_prefix = "ChronoLog";
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
//        if (CHRONOLOG_CONF->IS_SERVER) {
//            bind_functions();
//            rpc->start();
//        }
        LOGD("%s constructor finishes, object created@%p in thread PID=%d",
             typeid(*this).name(), this, getpid());
    }

    ~ChronoLogAdminRPCProxy() = default;

    bool LocalConnect(const std::string &uri, std::string &client_id) {
        LOGD("%s in ChronoLogAdminRPCProxy@%p called in PID=%d, with args: uri=%s",
             __FUNCTION__, this, getpid(), uri.c_str());
        ClientRegistryInfo record;
        record.addr_ = "127.0.0.1";
//        return ChronoLog::Singleton<ClientRegistryManager>::GetInstance()->add_client_record(client_id, record);
        extern std::shared_ptr<ClientRegistryManager> g_clientRegistryManager;
        g_clientRegistryManager->add_client_record(client_id, record);
        return true;
    }

    bool Connect(const std::string &uri, std::string &client_id) {
        LOGD("%s in ChronoLogAdminRPCProxy at addresss %p called in PID=%d, with args: uri=%s, client_id=%s",
             __FUNCTION__, this, getpid(), uri.c_str(), client_id.c_str());
        return CHRONOLOG_RPC_CALL_WRAPPER("Connect", 0, bool, uri, client_id);
    }

    bool LocalDisconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        extern std::shared_ptr<ClientRegistryManager> g_clientRegistryManager;
        return g_clientRegistryManager->remove_client_record(client_id, flags);
    }

    bool Disconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%d, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        return CHRONOLOG_RPC_CALL_WRAPPER("Disconnect", 0, bool, client_id, flags);
    }
    
    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case CHRONOLOG_THALLIUM_SOCKETS:
            case CHRONOLOG_THALLIUM_TCP:
            case CHRONOLOG_THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   const std::string &,
                                   std::string &)> connectFunc(
                        [this](auto && PH1, auto && PH2, auto && PH3) {
                            ThalliumLocalConnect(std::forward<decltype(PH1)>(PH1),
                                    std::forward<decltype(PH2)>(PH2),
                                    std::forward<decltype(PH3)>(PH3));
                        }
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   int &)> disconnectFunc(
                        [this](auto && PH1, auto && PH2, auto && PH3) {
                            ThalliumLocalDisconnect(std::forward<decltype(PH1)>(PH1),
                                    std::forward<decltype(PH2)>(PH2),
                                    std::forward<decltype(PH3)>(PH3));
                        }
                );
                
                rpc->bind("ChronoLogThalliumConnect", connectFunc);
                rpc->bind("ChronoLogThalliumDisconnect", disconnectFunc);
            }
        }
    }

    CHRONOLOG_THALLIUM_DEFINE(LocalConnect, (uri, client_id),
                    const std::string &uri, std::string &client_id)
    CHRONOLOG_THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string &client_id, int &flags)

private:
    ChronoLogCharStruct func_prefix;
    std::shared_ptr<ChronoLogRPC> rpc;
};

#endif //CHRONOLOG_CHRONOLOGADMINRPCPROXY_H