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
#include "ClientRegistryRecord.h"
#include "ClientRegistryManager.h"

class ChronoLogAdminRPCProxy {
public:
    ChronoLogAdminRPCProxy() : clientRegistryManager(ChronoLog::Singleton<ClientRegistryManager>::GetInstance()),
                               rpc(ChronoLog::Singleton<RPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_PORT)) {
        func_prefix = "ChronoLog";
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case THALLIUM_SOCKETS:
            case THALLIUM_TCP:
            case THALLIUM_ROCE:
                func_prefix += "Thallium";
                break;
        }
        if (CHRONOLOG_CONF->IS_SERVER) {
            bind_functions();
            rpc->start();
        }
    }

    ~ChronoLogAdminRPCProxy() = default;

    bool LocalConnect(const std::string &uri, std::string &client_id) {
        LOGD("%s is called in PID=%ld, with args: uri=%s",
             __FUNCTION__, getpid(), uri.c_str());
        ClientRegistryRecord record;
        return clientRegistryManager->connect(client_id, record);
    }

    bool Connect(const std::string &uri, std::string &client_id) {
        LOGD("%s is called in PID=%ld", __FUNCTION__, getpid());
        return RPC_CALL_WRAPPER("Connect", 0, bool, uri, client_id);
    }

    bool LocalDisconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%ld, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        return clientRegistryManager->disconnect(client_id);
    }

    bool Disconnect(const std::string &client_id, int &flags) {
        LOGD("%s is called in PID=%ld, with args: client_id=%s, flags=%d",
             __FUNCTION__, getpid(), client_id.c_str(), flags);
        return RPC_CALL_WRAPPER("Disconnect", 0, bool, client_id);
    }
    
    void bind_functions() {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            case THALLIUM_SOCKETS:
            case THALLIUM_TCP:
            case THALLIUM_ROCE: {
                std::function<void(const tl::request &,
                                   const std::string &,
                                   std::string &)> connectFunc(
                        std::bind(&ChronoLogAdminRPCProxy::ThalliumLocalConnect,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                std::function<void(const tl::request &,
                                   std::string &,
                                   int &)> disconnectFunc(
                        std::bind(&ChronoLogAdminRPCProxy::ThalliumLocalDisconnect,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3)
                );
                
                rpc->bind("ChronoLogThalliumConnect", connectFunc);
                rpc->bind("ChronoLogThalliumDisconnect", disconnectFunc);
            }
        }
    }

    THALLIUM_DEFINE(LocalConnect, (uri, client_id),
                    const std::string &uri, std::string &client_id)
    THALLIUM_DEFINE(LocalDisconnect, (client_id, flags), std::string &client_id, int &flags)

private:
    std::shared_ptr<ClientRegistryManager> clientRegistryManager;
    CharStruct func_prefix;
    std::shared_ptr<RPC> rpc;
};

#endif //CHRONOLOG_CHRONOLOGADMINRPCPROXY_H