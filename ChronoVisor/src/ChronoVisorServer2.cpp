//
// Created by kfeng on 7/19/22.
//

#include "ChronoVisorServer2.h"
#include "macro.h"

namespace ChronoVisor {
    ChronoVisorServer2::ChronoVisorServer2(const std::string &conf_file_path) {
        if (!conf_file_path.empty())
            CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
        init();
    }

    int ChronoVisorServer2::start() {
        LOGI("ChronoVisor server starting, listen on %d ports starting from %d ...", numPorts_, basePorts_);

        // bind functions first (defining RPC rorpcVisor_->bind_functions();

        // start engines (listening for incoming requests)
        ChronoLog::Singleton<ChronoLogRPCFactory>::GetInstance()->
                    GetRPC(CHRONOLOG_CONF->RPC_BASE_VISOR_PORT)->start();

        return 0;
    }

    void ChronoVisorServer2::init() {
        switch (CHRONOLOG_CONF->RPC_IMPLEMENTATION) {
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_SOCKETS()
                [[fallthrough]];
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_TCP() {
                protocol_ = CHRONOLOG_CONF->SOCKETS_CONF.string();
                break;
            }
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_ROCE() {
                protocol_ = CHRONOLOG_CONF->VERBS_CONF.string();
                break;
            }
        }
        baseIP_ = CHRONOLOG_CONF->RPC_VISOR_IP.string();
        basePorts_ = CHRONOLOG_CONF->RPC_BASE_VISOR_PORT;
        numPorts_ = CHRONOLOG_CONF->RPC_NUM_VISOR_PORTS;
        numStreams_ = CHRONOLOG_CONF->RPC_NUM_VISOR_SERVICE_THREADS;
        serverAddrVec_.reserve(CHRONOLOG_CONF->RPC_NUM_VISOR_PORTS);
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
        rpcVisor_ = ChronoLog::Singleton<RPCVisor>::GetInstance();
        clientRegistryManager_ = ChronoLog::Singleton<ClientRegistryManager>::GetInstance();
    }
}
