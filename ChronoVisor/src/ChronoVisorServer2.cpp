//
// Created by kfeng on 7/19/22.
//

#include "ChronoVisorServer2.h"
#include "macro.h"

namespace ChronoVisor {
    ChronoVisorServer2::ChronoVisorServer2() {
        CHRONOLOG_CONF->ConfigureDefaultServer("../../../../test/communication/server_list");
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            RPC_CALL_WRAPPER_THALLIUM_SOCKETS()
            [[fallthrough]];
            RPC_CALL_WRAPPER_THALLIUM_TCP() {
                protocol_ = CHRONOLOG_CONF->SOCKETS_CONF.string();
                break;
            }
            RPC_CALL_WRAPPER_THALLIUM_ROCE() {
                protocol_ = CHRONOLOG_CONF->VERBS_CONF.string();
                break;
            }
        }
        baseIP_ = CHRONOLOG_CONF->RPC_SERVER_IP.string();
        basePorts_ = CHRONOLOG_CONF->RPC_BASE_SERVER_PORT;
        numPorts_ = CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS;
        numStreams_ = CHRONOLOG_CONF->RPC_NUM_SERVICE_THREADS;
        serverAddrVec_.reserve(CHRONOLOG_CONF->RPC_NUM_SERVER_PORTS);
        for (int i = 0; i < numPorts_; i++) {
            std::string server_addr = protocol_ + "://" +
                    baseIP_ + ":" +
                    std::to_string(basePorts_ + i);
            serverAddrVec_.emplace_back(std::move(server_addr));
        }
        engineVec_.reserve(numPorts_);
        midVec_.reserve(numPorts_);
        pTimeManager = new TimeManager();
        chronicleMetaDirectory_ = ChronoLog::Singleton<ChronicleMetaDirectory>::GetInstance();
        adminRpcProxy_ = ChronoLog::Singleton<ChronoLogAdminRPCProxy>::GetInstance();
        metadataRPCProxy_ = ChronoLog::Singleton<ChronicleMetadataRPCProxy>::GetInstance();
        clientRegistryManager_ = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
    }

    int ChronoVisorServer2::start() {
        LOGI("ChronoVisor server starting, listen on %d ports starting from %d ...", numPorts_, basePorts_);

        // bind functions first (defining RPC routines on engines)
        adminRpcProxy_->bind_functions();
        metadataRPCProxy_->bind_functions();

        // start engines (listening for incoming requests)
        ChronoLog::Singleton<RPCFactory>::GetInstance()->GetRPC(CHRONOLOG_CONF->RPC_BASE_SERVER_PORT)->start();

        return 0;
    }
}