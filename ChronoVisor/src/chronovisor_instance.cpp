#include <signal.h>
//#include "ClocksourceManager.h"
#include <unistd.h>

#include "cmd_arg_parse.h"
#include "KeeperRegistry.h"
#include "chrono_monitor.h"
#include "VisorClientPortal.h"

volatile sig_atomic_t keep_running = true;

void sigterm_handler(int)
{
    LOG_INFO("Received SIGTERM, start shutting down.");
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
    int result = chronolog::chrono_monitor::initialize(confManager.VISOR_CONF.VISOR_LOG_CONF.LOGTYPE
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGFILE
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGLEVEL
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGNAME
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGFILESIZE
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.LOGFILENUM
                                                       , confManager.VISOR_CONF.VISOR_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[chronovisor_instance] Running Chronovisor Server.");

    chronolog::VisorClientPortal theChronoVisorPortal;
    chronolog::KeeperRegistry keeperRegistry;

    // ChronoVisor::ChronoVisorServer2 visor(confManager);
    keeperRegistry.InitializeRegistryService(confManager);//provider_id=22);
    // visor.start(&keeperRegistry);
    theChronoVisorPortal.StartServices(confManager, &keeperRegistry);

    // If services do not start successfully there should a graceful exit(-1) here 
    /////
    LOG_INFO("[chronovisor_instance] ChronoVisor Running...");
    while(keep_running)
    {
        sleep(10);
    }

    theChronoVisorPortal.ShutdownServices();
    keeperRegistry.ShutdownRegistryService();
    LOG_INFO("[chronovisor_instance] ChronoVisor shutdown.");
    return 0;
}
