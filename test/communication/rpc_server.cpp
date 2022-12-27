//
// Created by kfeng on 3/30/22.
//

#include <rpc.h>
#include <ClientRegistryManager.h>
#include "RPCVisor.h"
#include "global_var_visor.h"
#include "log.h"

std::shared_ptr<ClientRegistryManager> clientRegistryManager = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
std::mutex clientRegistryMutex_;

int main() {
    CHRONOLOG_CONF->ConfigureDefaultServer("../../../test/communication/server_list");
    std::unique_ptr<RPCVisor> rpcProxy = std::make_unique<RPCVisor>();
    rpcProxy->bind_functions();
//    ChronicleMetadataRPCProxy metadataRPCProxy;
    ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT)->start();

    return 0;
}