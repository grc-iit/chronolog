#include "ChronoVisorServer2.h"
#include "macro.h"

#include "KeeperRegistry.h"

namespace ChronoVisor
{
ChronoVisorServer2::ChronoVisorServer2(const std::string &conf_file_path)
{
    if(!conf_file_path.empty())
        CHRONOLOG_CONF->LoadConfFromJSONFile(conf_file_path);
    init();
}

ChronoVisorServer2::ChronoVisorServer2(const ChronoLog::ConfigurationManager &conf_manager)
{
    CHRONOLOG_CONF->SetConfiguration(conf_manager);
    CHRONOLOG_CONF->ROLE = CHRONOLOG_VISOR;
    init();
}

int ChronoVisorServer2::start(chronolog::KeeperRegistry*keeper_registry)
{
    rpcVisor_ = ChronoLog::Singleton <RPCVisor>::GetInstance(keeper_registry);

    LOGI("ChronoVisor server starting, listen on %d ports starting from %d ...", numPorts_, basePorts_);

    // bind functions first (defining RPC routines on engines)
    rpcVisor_->bind_functions();

    // start engines (listening for incoming requests)
    rpcVisor_->Visor_start();

    return 0;
}

void ChronoVisorServer2::init()
{
    //pClocksourceManager_ = ClocksourceManager::getInstance();
    //pClocksourceManager_->setClocksourceType(CHRONOLOG_CONF->CLOCKSOURCE_TYPE);
    //CHRONOLOG_CONF->ROLE = CHRONOLOG_VISOR;
}
}
