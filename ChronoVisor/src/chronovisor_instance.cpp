#include <signal.h>
//#include "ClocksourceManager.h"
#include <unistd.h>

#include "cmd_arg_parse.h"
#include "KeeperRegistry.h"
#include "log.h"
#include "VisorClientPortal.h"

volatile sig_atomic_t keep_running = true;

void sigterm_handler(int)
{
    LOGI("Received SIGTERM, start shutting down.");
    keep_running = false;
}


///////////////////////////////////////////////
int main(int argc, char**argv)
{
    signal(SIGTERM, sigterm_handler);

    // IMPORTANT NOTE!!!!
    // we need to instantiate Client Registry,
    // MetadataDirectory, & KeeperRegistry
    // without initializing any of the related RPC communication objects
    // to ensure that the main thread is the only one running at this point.
    // After they are instantiated and tied together
    // we can start RPC engines initialization that would start
    // a bunch of new threads....
    //
    // If we can't ensure the instantiation of registries on the main thread
    // we'd need to add static mutex protection to all the registry singleton objects

    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = Logger::initialize(confManager.VISOR_CONF.VISOR_LOG_CONF.LOGTYPE
                                    , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGFILE
                                    , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGLEVEL
                                    , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGNAME);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOGI("[chronovisor_instance] Running Chronovisor Server.");

    chronolog::VisorClientPortal theChronoVisorPortal;
    chronolog::KeeperRegistry keeperRegistry;

    // ChronoVisor::ChronoVisorServer2 visor(confManager);
    keeperRegistry.InitializeRegistryService(confManager);//provider_id=22);
    // visor.start(&keeperRegistry);
    theChronoVisorPortal.StartServices(confManager, &keeperRegistry);

    /////
    LOGI("[chronovisor_instance] ChronoVisor Running...");
    while(keep_running)
    {
        sleep(10);
    }

    theChronoVisorPortal.ShutdownServices();
    keeperRegistry.ShutdownRegistryService();
    LOGI("[chronovisor_instance] ChronoVisor shutdown.");
    return 0;
}
