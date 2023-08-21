
#include <signal.h>
//#include "ClocksourceManager.h"
#include <unistd.h>

#include "cmd_arg_parse.h"
#include "KeeperRegistry.h"

#include "VisorClientPortal.h"

volatile sig_atomic_t keep_running = true;

void sigterm_handler (int)
{
    std::cout << "Received SIGTERM, starrt shutting down "<<std::endl;

    keep_running = false;
    return;
}


///////////////////////////////////////////////
int main(int argc, char** argv)
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

    std::string default_conf_file_path = "./default_conf.json";
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if (conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }

    ChronoLog::ConfigurationManager confManager(conf_file_path);

    chronolog::VisorClientPortal theChronoVisorPortal; // confManager);

    chronolog::KeeperRegistry keeperRegistry;

   // ChronoVisor::ChronoVisorServer2 visor(confManager);

    keeperRegistry.InitializeRegistryService(confManager);//provider_id=22);

//    visor.start(&keeperRegistry);

    theChronoVisorPortal.StartServices(confManager, &keeperRegistry);


    /////

    while( keep_running)
    {
       sleep(10);
    }


    theChronoVisorPortal.ShutdownServices();
    keeperRegistry.ShutdownRegistryService();

    return 0;
}
