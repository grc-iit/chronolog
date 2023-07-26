
//#include "ChronoVisorServer2.h"
#include <unistd.h>

#include "KeeperRegistry.h"

#include "VisorClientPortal.h"

#include "cmd_arg_parse.h"

///////////////////////////////////////////////
int main(int argc, char** argv) 
{

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

    keeperRegistry.InitializeRegistryService( confManager);//provider_id=22);

//    visor.start(&keeperRegistry);

    theChronoVisorPortal.StartServices(confManager, &keeperRegistry);

    
    /////

    while (!keeperRegistry.is_shutting_down())
    {
        sleep(60);
    }
	

    keeperRegistry.ShutdownRegistryService();
    return 0;
}