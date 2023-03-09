
#include <iostream>

#include <thallium.hpp>

#include "KeeperRegistry.h"
#include "KeeperRegistryService.h"
#include "DataCollectionClient.h"
// this file allows testing of the KeeperRegistryService and KeeperRegistry classes 
// as the standalone process without the rest of the ChronoVizor modules 
//

#define KEEPER_REGISTRY_SERVICE_NA_STRING  "ofi+sockets://127.0.0.1:1234"
#define KEEPER_REGISTRY_SERVICE_PROVIDER_ID	25

/////////////////////////

namespace thallium = tl;

namespace chronolog
{

int KeeperRegistry::InitializeRegistryService(uint16_t service_provider_id)
{
    std::lock_guard<std::mutex> lock(registryLock);

    keeperRegistryService =
	    KeeperRegistryService::CreateKeeperRegistryService(registryEngine,service_provider_id, *this);

    registryState = INITIALIZED;
    return 1;

}
/////////////////

int KeeperRegistry::ShutdownRegistryService()
{
    if(is_shutting_down())
    {return 1;}

    std::cout<<"KeeperRegistry: shutting down ...."<<std::endl;

    std::lock_guard<std::mutex> lock(registryLock);
    //check again after the lock is aquired
    if(is_shutting_down())
    { return 1;}
  
    registryState = SHUTTING_DOWN;

    // send out shutdown instructions to 
    // all the registered keeper processes
    // then drain the registry
    if( !keeperProcessRegistry.empty() )
    {
    	for ( auto process : keeperProcessRegistry)
    	{
	   if ( process.second.keeperCollectionClient != nullptr)
	   {  
	      std::cout<<"KeeperRegistry: sending shutdown to keeper {"<<process.second.idCard<<"}"<<std::endl;
	      process.second.keeperCollectionClient->shutdown_collection();
              delete process.second.keeperCollectionClient;
           }
	}
	keeperProcessRegistry.clear();
     }
    std::cout<<"KeeperRegistry: shutting down RegistryService"<<std::endl;

    if( nullptr != keeperRegistryService)
    {   delete keeperRegistryService;   }

return 1;
}

KeeperRegistry::~KeeperRegistry()
{  
   ShutdownRegistryService();
}
/////////////////
	

int KeeperRegistry::registerKeeperProcess( KeeperRegistrationMsg const& keeper_reg_msg)
{
	   if(is_shutting_down())
	   { return 0;}

	   std::lock_guard<std::mutex> lock(registryLock);
	   //re-check state after ther lock is aquired
	   if(is_shutting_down())
	   { return 0;}

	   KeeperIdCard keeper_id_card = keeper_reg_msg.getKeeperIdCard();
	   ServiceId admin_service_id = keeper_reg_msg.getAdminServiceId();

	   auto insert_return = keeperProcessRegistry.insert( std::pair<std::pair<uint32_t,uint16_t>,KeeperProcessEntry>
		( std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()),
			   KeeperProcessEntry(keeper_id_card, admin_service_id) )
		);
	   if( false == insert_return.second)
	   {//INNA: revisit this paragraph... 
	      std::cout<<"KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} found existing keeperProcess, reseting the process record"<< std::endl;
	     // insert_return.first is the position to the current keeper record with the same KeeperIdCard
	     // this is probably the case of keeper process restart ... reset the record  entry
	         if( (*insert_return.first).second.keeperCollectionClient != nullptr)
		 { delete (*insert_return.first).second.keeperCollectionClient; }
	      
	         (*insert_return.first).second.reset();
		   return 0;
	   }

           //create a client of Keeper's DataCollectionService listenning at adminServiceId
           std::string service_na_string("ofi+sockets://");
	   service_na_string = admin_service_id.getIPasDottedString(service_na_string)
                             + ":"+ std::to_string(admin_service_id.port);
           try
	   {
              DataCollectionClient * collectionClient = DataCollectionClient::CreateDataCollectionClient(registryEngine,service_na_string, 
			      admin_service_id.provider_id);
	
	      (*insert_return.first).second.keeperCollectionClient = collectionClient;

	      std::cout<<"KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} created DataCollectionClient for service {"<<service_na_string
		   <<": provider_id="<<admin_service_id.provider_id<<"}"<<std::endl;
	   }
	   catch( tl::exception  const& ex)
	   {
	      std::cout <<"KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} failed to create DataCollectionClient for {"<<service_na_string
		   <<": provider_id="<<admin_service_id.provider_id<<"}"<<std::endl;

	   } 
    	   // now that communnication with the Keeper is established and we still holding registryLock
           // update registryState in case this is the first KeeperProcess registration
	   registryState = RUNNING;

	   std::cout << "KeeperRegistry : RUNNING with {" << keeperProcessRegistry.size()<<"} KeeperProcesses"<<std::endl;

return 1;
}
/////////////////

int KeeperRegistry::unregisterKeeperProcess( KeeperIdCard const & keeper_id_card) 
       {
           if(is_shutting_down())
           {  return 0;}

	   std::lock_guard<std::mutex> lock(registryLock);
           //check again after the lock is aquired
           if(is_shutting_down())
           {  return 0;}

	   // stop & delete keeperCollectionClient before erasing keeper_process entry
           auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   if(keeper_process_iter != keeperProcessRegistry.end())
	   {
	   // delete keeperCollectionClient before erasing keeper_process entry
	      if( (*keeper_process_iter).second.keeperCollectionClient != nullptr)
	      {  delete (*keeper_process_iter).second.keeperCollectionClient; }
	      keeperProcessRegistry.erase(keeper_process_iter);
	   }
	   // now that we are still holding registryLock
	   // update registryState if needed
	   if( !is_shutting_down() && (0 == keeperProcessRegistry.size()))
	   { registryState = INITIALIZED;  
	     std::cout<<"KeeperRegistry: state {INITIALIZED}" << " with {"<< keeperProcessRegistry.size()<<"} KeeperProcesses"<<std::endl;
	   }

	   return 1;
	}
/////////////////

void KeeperRegistry::updateKeeperProcessStats(KeeperStatsMsg const & keeperStatsMsg) 
	{
           if(is_shutting_down())
           {  return; }

	   std::lock_guard<std::mutex> lock(registryLock);
           if(is_shutting_down())
           {  return; }
	   KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();
	   auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	   if(keeper_process_iter == keeperProcessRegistry.end())
	   {    // however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
		// we should probably log a warning here...
		return;
	   }
   	   KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	  // keeper_process.lastStatsTime = std::chrono::steady_clock::now();
	   keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();

	}
/////////////////

std::vector<KeeperIdCard> & KeeperRegistry::getActiveKeepers( std::vector<KeeperIdCard> & keeper_id_cards) 
	{  //the process of keeper selection will probably get more nuanced; 
	   //for now just return all the keepers registered
           if(is_shutting_down())
           {  return keeper_id_cards;}

	   std::lock_guard<std::mutex> lock(registryLock);
           if(is_shutting_down())
           {  return keeper_id_cards;}

	   keeper_id_cards.clear();
	   for (auto keeperProcess : keeperProcessRegistry )
		   keeper_id_cards.push_back(keeperProcess.second.idCard);

	   return keeper_id_cards;
	}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStart( std::vector<KeeperIdCard> const& vectorOfKeepers
		, ChronicleName const& chronicle, StoryName const& story, StoryId const& storyId)
{ 
   int ret_status = 1;
   
   if (!is_running())
   {
      std::cout<<"Registry has no Keeper processes to start story recording" << std::endl;
      return -1;
   }

    std::lock_guard<std::mutex> lock(registryLock);
   if (!is_running())
   {   return -1;}

    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for ( KeeperIdCard keeper_id_card : vectorOfKeepers)
    {
        auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
        if(keeper_process_iter == keeperProcessRegistry.end())
        {
	  std::cout<<"WARNING: Registry faield to find Keeper with {"<<keeper_id_card<<"}"<<std::endl;
	  continue;
	}
   	KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	std::cout<<"found keeper_process:"<< &keeper_process<<" "<<keeper_process.idCard <<" "<<keeper_process.adminServiceId<<keeper_process.keeperCollectionClient<<std::endl;;
	if (nullptr == keeper_process.keeperCollectionClient)
	{ 
	  std::cout<<"WARNING: Registry record for{"<<keeper_id_card<<"} is missing keeperCollectionClient"<<std::endl;
	  continue;
	}
	try
	{
	   int rpc_return = keeper_process.keeperCollectionClient->send_start_story_recording(chronicle, story, storyId);
	   if (rpc_return <0)
	   {
	      std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"}"<<std::endl;
	      continue;
	   }
	   std::cout << "Registry notified keeper {"<<keeper_id_card<<"} to start recording story {"<<storyId<<"}"<<std::endl;
	   keepers_left_to_notify--;
	}
	catch(thallium::exception const& ex)
	{
	  std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"} details: "<<std::endl;
	  continue;
	}
    }

    if ( keepers_left_to_notify == vectorOfKeepers.size())
    {
	  std::cout<<"ERROR: Registry failed to notify the keepers to start recording story {"<<storyId<<"}"<<std::endl;
	  return -1;
    }

   return ret_status;
}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStop(std::vector<KeeperIdCard> const& vectorOfKeepers, StoryId const& storyId)
{
    int ret_status = 1;
    if (!is_running())
    {
       std::cout<<"Registry has no Keeper processes to notify of story release" << std::endl;
       return -1;
    }
    
    std::lock_guard<std::mutex> lock(registryLock);
    if(!is_running())
    {  return -1; }

    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for (KeeperIdCard keeper_id_card : vectorOfKeepers)
    {
        auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
        if(keeper_process_iter == keeperProcessRegistry.end())
        {
	  std::cout<<"WARNING: Registry faield to find Keeper with {"<<keeper_id_card<<"}"<<std::endl;
	  continue;
	}
   	KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	if (nullptr == keeper_process.keeperCollectionClient)
	{ 
	  std::cout<<"WARNING: Registry record for{"<<keeper_id_card<<"} is missing keeperCollectionClient"<<std::endl;
	  continue;
	}
	try
	{
	   int rpc_return = keeper_process.keeperCollectionClient->send_stop_story_recording(storyId);
	   if (rpc_return <0)
	   {
	      std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"}"<<std::endl;
	      continue;
	   }
	   std::cout << "Registry notified keeper {"<<keeper_id_card<<"} to stop recording story {"<<storyId<<"}"<<std::endl;
	   keepers_left_to_notify--;
	}
	catch(thallium::exception const& ex)
	{
	  std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"} details: "<<std::endl;
	  continue;
	}
    }

    if ( keepers_left_to_notify == vectorOfKeepers.size())
    {
	  std::cout<<"ERROR: Registry failed to notify the keepers to stop recording story {"<<storyId<<"}"<<std::endl;
	  return -1;
    }
  return ret_status;
}


}//namespace chronolog


///////////////////////////////////////////////
int main(int argc, char** argv) {

    chronolog::RegistryConfiguration registryConfig;
    registryConfig.REGISTRY_SERVICE_PROTOCOL = std::string(argv[1]);
    registryConfig.REGISTRY_SERVICE_IP=std::string(argv[2]);
    registryConfig.REGISTRY_SERVICE_PORT= atoi(argv[3]);
    registryConfig.REGISTRY_SERVICE_PROVIDER_ID =atoi(argv[4]);
    registryConfig.SERVICE_THREAD_COUNT=2;

    uint16_t provider_id = KEEPER_REGISTRY_SERVICE_PROVIDER_ID;

    margo_instance_id margo_id=margo_init(KEEPER_REGISTRY_SERVICE_NA_STRING, MARGO_SERVER_MODE, 1, 2);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"KeeperRegistryService:margo_instance initialized"<<std::endl;

   tl::engine keeper_reg_engine(margo_id);
 
    std::cout << "Starting KeeperRegistryService  at address " << keeper_reg_engine.self()
        << " with provider id " << provider_id << std::endl;

    chronolog::KeeperRegistry keeperRegistry (keeper_reg_engine);

    keeperRegistry.InitializeRegistryService(provider_id);

    
    std::string chronicle ="chronicle_";
    std::string story= "story_";
    uint64_t story_id = 0;
    std::vector<chronolog::KeeperIdCard> vectorOfKeepers;
    while( !keeperRegistry.is_shutting_down())
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
	

    sleep(180);
    keeperRegistry.ShutdownRegistryService();

    keeper_reg_engine.finalize();
  
    return 0;
}
