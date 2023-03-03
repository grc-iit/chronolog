//
// Created by kfeng on 7/19/22.
//

#include "ChronoVisorServer2.h"
#include "macro.h"
#include "ACL_List.h"

namespace ChronoVisor {
    ChronoVisorServer2::ChronoVisorServer2(const std::string &conf_file_path) {
        if (!conf_file_path.empty())
            CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
        init();
    }

    ChronoVisorServer2::ChronoVisorServer2(const ChronoLog::ConfigurationManager &conf_manager) {
        CHRONOLOG_CONF->SetConfiguration(conf_manager);
        init();
    }

    int ChronoVisorServer2::start() {
        LOGI("ChronoVisor server starting, listen on %d ports starting from %d ...", numPorts_, basePorts_);

        // bind functions first (defining RPC routines on engines)
        rpcVisor_->bind_functions();

	if(use_acl_db)
	{
	  std::string filename = "clients.json";
	  ACL_DB *ad = CreateACLDB(filename);
	  rpcVisor_->set_acl_db(ad);
	}
        // start engines (listening for incoming requests)
        rpcVisor_->Visor_start();

        return 0;
    }

    void ChronoVisorServer2::init() {
        pClocksourceManager_ = ClocksourceManager::getInstance();
        pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);
        CHRONOLOG_CONF->ROLE = CHRONOLOG_VISOR;
        switch (CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.RPC_IMPLEMENTATION) {
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_SOCKETS()
                [[fallthrough]];
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_TCP() {
                protocol_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
                break;
            }
            CHRONOLOG_RPC_CALL_WRAPPER_THALLIUM_ROCE() {
                protocol_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.PROTO_CONF.string();
                break;
            }
        }
        baseIP_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_IP.string();
        basePorts_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_BASE_PORT;
        numPorts_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS;
        numStreams_ = CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_SERVICE_THREADS;
        serverAddrVec_.reserve(CHRONOLOG_CONF->RPC_CONF.CLIENT_VISOR_CONF.VISOR_END_CONF.VISOR_PORTS);
        for (int i = 0; i < numPorts_; i++) {
            std::string server_addr = protocol_ + "://" +
                                      baseIP_ + ":" +
                                      std::to_string(basePorts_ + i);
            serverAddrVec_.emplace_back(std::move(server_addr));
        }
        engineVec_.reserve(numPorts_);
        midVec_.reserve(numPorts_);
        rpcVisor_ = ChronoLog::Singleton<RPCVisor>::GetInstance();
	use_acl_db = false;
    }
}
