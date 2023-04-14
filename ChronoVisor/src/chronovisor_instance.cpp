
#include "ChronoVisorServer2.h"

#include "KeeperRegistry.h"

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

    ChronoLog::ConfigurationManager confManager("./default_conf.json");

    chronolog::KeeperRegistry keeperRegistry;

    keeperRegistry.InitializeRegistryService( confManager);//provider_id=22);
////
    ChronoVisor::ChronoVisorServer2 visor(confManager);
    visor.start();
    /////

    std::string chronicle ="chronicle_";
    std::string story= "story_";
    uint64_t story_id = 0;

    std::vector<chronolog::KeeperIdCard> vectorOfKeepers;
    /* while( !keeperRegistry.is_shutting_down())
    {

       if (keeperRegistry.is_running())
       {  vectorOfKeepers.clear();
	  vectorOfKeepers = keeperRegistry.getActiveKeepers(vectorOfKeepers);

       	  story_id++;
          keeperRegistry.notifyKeepersOfStoryRecordingStart( vectorOfKeepers,
	               chronicle +std::to_string(story_id), story +std::to_string(story_id), story_id);
          if (story_id >5)
          { break;}

       }
       sleep(60);
    }
*/	

    sleep(10);
    keeperRegistry.ShutdownRegistryService();
  
    return 0;
}
