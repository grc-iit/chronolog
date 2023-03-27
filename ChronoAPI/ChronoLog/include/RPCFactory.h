//
// Created by kfeng on 4/3/22.
//

#ifndef CHRONOLOG_RPCFACTORY_H
#define CHRONOLOG_RPCFACTORY_H

#include <unordered_map>
#include <memory>
#include "rpc.h"

class ChronoLogRPCFactory {
private:
    std::unordered_map<uint16_t, std::shared_ptr<ChronoLogRPC>> rpcs;
public:
    ChronoLogRPCFactory() : rpcs() {}

    std::shared_ptr<ChronoLogRPC> GetRPC(uint16_t server_port) {
        auto iter = rpcs.find(server_port);
        if (iter != rpcs.end()) return iter->second;
        auto temp = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = server_port;
        auto rpc = std::make_shared<ChronoLogRPC>();
        rpcs.emplace(server_port, rpc);
        CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT = temp;
        return rpc;
    }
};

#endif //CHRONOLOG_RPCFACTORY_H
