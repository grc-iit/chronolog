//
// Created by kfeng on 3/30/22.
//

#include <rpc.h>
#include <ClientRegistryManager.h>
#include "RPCVisor.h"
#include "global_var_visor.h"

int main() {
    CHRONOLOG_CONF->LoadConfFromJSONFile("./default_conf_server.json");
    std::unique_ptr<RPCVisor> rpcProxy = std::make_unique<RPCVisor>();
    rpcProxy->bind_functions();
//    ChronicleMetadataRPCProxy metadataRPCProxy;
    ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_BASE_VISOR_PORT)->start();

    return 0;
}