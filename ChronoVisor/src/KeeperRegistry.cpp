
#include <iostream>

#include <thallium.hpp>

#include "KeeperRegistry.h"
#include "KeeperRegistryService.h"
#include "DataStoreAdminClient.h"
#include "ConfigurationManager.h"
/////////////////////////

namespace tl = thallium;
namespace chl = chronolog;

namespace chronolog
{

int KeeperRegistry::InitializeRegistryService(ChronoLog::ConfigurationManager const& confManager )
{
    int status = CL_ERR_UNKNOWN;

    std::lock_guard<std::mutex> lock(registryLock);

    if(registryState != UNKNOWN)
    { return CL_SUCCESS; }

    try
    {
        // initialise thalium engine for KeeperRegistryService
        std::string KEEPER_REGISTRY_SERVICE_NA_STRING =
                    confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF
                    + "://" + confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP
                    + ":" + std::to_string(confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT);

        uint16_t provider_id = confManager.VISOR_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

        margo_instance_id margo_id=margo_init(KEEPER_REGISTRY_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 2);

        if(MARGO_INSTANCE_NULL == margo_id)
        {  
             std::cout<<"KeeperRegistryService: Failed to initialize margo_instance"<<std::endl;
             return -1;
         }
         std::cout<<"KeeperRegistryService:margo_instance initialized with NA_STRING"
             << "{"<<KEEPER_REGISTRY_SERVICE_NA_STRING<<"}" <<std::endl;

        registryEngine =  new tl::engine(margo_id);
 
        std::cout << "Starting KeeperRegistryService  at address " << registryEngine->self()
        << " with provider id " << provider_id << std::endl;



        keeperRegistryService = KeeperRegistryService::CreateKeeperRegistryService(*registryEngine, provider_id, *this);

        registryState = INITIALIZED;
        status = CL_SUCCESS;
    }
    catch(tl::exception const& ex)
    {
        std::cout<<"ERROR: Failed to start KeeperRegistryService "<<std::endl;
    }
    
    return status;

}
/////////////////

int KeeperRegistry::ShutdownRegistryService()
{
    std::cout<<"KeeperRegistry: shutting down ...."<<std::endl;

    std::lock_guard<std::mutex> lock(registryLock);
   
    if(is_shutting_down())
    { return CL_SUCCESS;}
  
    registryState = SHUTTING_DOWN;

    // send out shutdown instructions to 
    // all active keeper processes
    // then drain the registry
    while( !keeperProcessRegistry.empty() )
    {
        for ( auto process_iter= keeperProcessRegistry.begin(); process_iter !=keeperProcessRegistry.end(); )
        {
             std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
            if ( (*process_iter).second.active)
            {  
                std::cout<<"KeeperRegistry: sending shutdown to keeper {"<<(*process_iter).second.idCard<<"}"<<std::endl;
                (*process_iter).second.keeperAdminClient->shutdown_collection();

	        (*process_iter).second.active = false;

	        if( (*process_iter).second.keeperAdminClient != nullptr)
            {
                std::time_t delayedExitTime =std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now()+std::chrono::seconds(3));
                std::cout <<"KeeperRegistry::register keeper {"<<(*process_iter).second.idCard<<"}  found old instance of DataStoreAdminclient : current_time="<<ctime(&current_time)<<"} delayedExitTime= " << std::ctime(&delayedExitTime)<<std::endl;
    
	        (*process_iter).second.delayedExitClients.push_back(std::pair<std::time_t, DataStoreAdminClient*>
                ( delayedExitTime, (*process_iter).second.keeperAdminClient) );
            (*process_iter).second.keeperAdminClient = nullptr;
            }
            }

        while( !(*process_iter).second.delayedExitClients.empty() 
        && ( current_time >= (*process_iter).second.delayedExitClients.front().first ))
        {
            auto dataStoreClientPair = (*process_iter).second.delayedExitClients.front();
            std::cout <<"INFO: KeeperRegistry::finalize destroys old client for {"<<(*process_iter).second.idCard<<"} delayedTime=" << ctime(&(dataStoreClientPair.first))<<std::endl;
            if(dataStoreClientPair.second != nullptr)
            {  delete dataStoreClientPair.second; }
            (*process_iter).second.delayedExitClients.pop_front();
        }

        if( (*process_iter).second.delayedExitClients.empty() )
        {
            std::cout <<"INFO: KeeperRegistry::registerKeeperProcess destroys old entry for {"<<(*process_iter).second.idCard<<"}"<<std::endl;
            process_iter = keeperProcessRegistry.erase(process_iter);
        }
        else
        {
            std::cout <<"INFO: KeeperRegistry::finalize : AdminClient for {"<<(*process_iter).second.idCard<<"} is not yet dismantled"<<std::endl;
            ++process_iter;
        }

        }
    }
    std::cout<<"KeeperRegistry: shutting down RegistryService"<<std::endl;

    if( nullptr != keeperRegistryService)
    {   delete keeperRegistryService;   }

return CL_SUCCESS;
}

KeeperRegistry::~KeeperRegistry()
{  
   ShutdownRegistryService();
   registryEngine->finalize();
   delete registryEngine;
}
/////////////////
	

int KeeperRegistry::registerKeeperProcess( KeeperRegistrationMsg const& keeper_reg_msg)
{

	if(is_shutting_down())
	{ return CL_ERR_UNKNOWN;}

	std::lock_guard<std::mutex> lock(registryLock);
	   //re-check state after ther lock is aquired
	if(is_shutting_down())
	{ return CL_ERR_UNKNOWN;}

	KeeperIdCard keeper_id_card = keeper_reg_msg.getKeeperIdCard();
	ServiceId admin_service_id = keeper_reg_msg.getAdminServiceId();
	// unlikely but possible that the Registry still retains the record of the previous re-incarnation of hte Keeper process 
    // running on the same host... check for this case and clean up the leftover record...
    auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	if(keeper_process_iter != keeperProcessRegistry.end())
	{
        // must be a case of the KeeperProcess exiting without unregistering or some unexpected break in communication...
        // start delayed destruction process for hte lingering keeperAdminclient to be safe...
        
        std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
	    (*keeper_process_iter).second.active = false;

	    if( (*keeper_process_iter).second.keeperAdminClient != nullptr)
        {
    
        std::time_t delayedExitTime =std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now()+std::chrono::seconds(3));
        std::cout <<"KeeperRegistry::register keeper {"<<keeper_id_card<<"}  found old instance of DataStoreAdminclient : current_time="<<ctime(&current_time)<<"} delayedExitTime= " << std::ctime(&delayedExitTime)<<std::endl;
    
	    (*keeper_process_iter).second.delayedExitClients.push_back(std::pair<std::time_t, DataStoreAdminClient*>
             ( delayedExitTime, (*keeper_process_iter).second.keeperAdminClient) );
        (*keeper_process_iter).second.keeperAdminClient = nullptr;
        }
 
        while( !(*keeper_process_iter).second.delayedExitClients.empty() 
        && ( current_time >= (*keeper_process_iter).second.delayedExitClients.front().first ))
        {
            auto dataStoreClientPair = (*keeper_process_iter).second.delayedExitClients.front();
            std::cout <<"INFO: KeeperRegistry::registerKeeperProcess destroys old client for {"<<(*keeper_process_iter).second.idCard<<"} delayedTime=" << ctime(&(dataStoreClientPair.first))<<std::endl;
            if(dataStoreClientPair.second != nullptr)
            {  delete dataStoreClientPair.second; }
            (*keeper_process_iter).second.delayedExitClients.pop_front();
        }

        if( (*keeper_process_iter).second.delayedExitClients.empty() )
        {
            std::cout <<"INFO: KeeperRegistry::registerKeeperProcess destroys old entry for {"<<(*keeper_process_iter).second.idCard<<"}"<<std::endl;
            keeperProcessRegistry.erase(keeper_process_iter);
        }
        else
        {
            std::cout <<"INFO: KeeperRegistry::registerKeeper {"<<keeper_id_card<<"} cant's proceed as previous AdminClient is not yet dismantled"<<std::endl;
            return CL_ERR_UNKNOWN; 
        }
            
    }
    
    //create a client of Keeper's DataStoreAdminService listenning at adminServiceId
    std::string service_na_string("ofi+sockets://");
	service_na_string = admin_service_id.getIPasDottedString(service_na_string)
                             + ":"+ std::to_string(admin_service_id.port);
              
    DataStoreAdminClient * collectionClient = DataStoreAdminClient::CreateDataStoreAdminClient(*registryEngine,service_na_string, admin_service_id.provider_id);
    if(nullptr == collectionClient)
    {    	
	    std::cout <<"ERROR: KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} failed to create DataStoreAdminClient for {"
                <<service_na_string <<": provider_id="<<admin_service_id.provider_id<<"}"<<std::endl;
        return CL_ERR_UNKNOWN;
    }

    //now create a new KeeperRecord with the new DataAdminclient
	auto insert_return = keeperProcessRegistry.insert( std::pair<std::pair<uint32_t,uint16_t>,KeeperProcessEntry>
		( std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()),
			   KeeperProcessEntry(keeper_id_card, admin_service_id) )
		);
	if( false == insert_return.second)
	{
	    std::cout <<"ERROR:KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} failed to registration"<<std::endl;
        delete collectionClient;
        return CL_ERR_UNKNOWN;

    }


	(*insert_return.first).second.keeperAdminClient = collectionClient;
	(*insert_return.first).second.active = true;

	std::cout<<"KeeperRegistry: registerKeeper {"<<keeper_id_card<<"} created DataStoreAdminClient for service {"<<service_na_string
		   <<": provider_id="<<admin_service_id.provider_id<<"}"<<std::endl;

    // now that communnication with the Keeper is established and we still holding registryLock
    // update registryState in case this is the first KeeperProcess registration
	if( keeperProcessRegistry.size() >0)
    { registryState = RUNNING; }

	std::cout << "KeeperRegistry : RUNNING with {" << keeperProcessRegistry.size()<<"} KeeperProcesses"<<std::endl;

return CL_SUCCESS;
}
/////////////////

int KeeperRegistry::unregisterKeeperProcess( KeeperIdCard const & keeper_id_card) 
{
    if(is_shutting_down())
	   { return CL_ERR_UNKNOWN;}

	std::lock_guard<std::mutex> lock(registryLock);
    //check again after the lock is acquired
    if(is_shutting_down())
	   { return CL_ERR_UNKNOWN;}

	// stop & delete keeperAdminClient before erasing keeper_process entry
    auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	if(keeper_process_iter != keeperProcessRegistry.end())
	{
        // we mark the keeperProcessEntry as inactive and set the time it would be safe to delete. 
        // we delay the destruction of the keeperEntry & keeperAdminClient by 5 secs
        // to prevent the case of deleting the keeperAdminClient while it might be waiting for rpc response on the 
        // other thread
        
	    (*keeper_process_iter).second.active = false;

	    if( (*keeper_process_iter).second.keeperAdminClient != nullptr)
        {
    
        std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());
        std::time_t delayedExitTime =std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now()+std::chrono::seconds(30));
        std::cout <<"KeeperRegistry::unregistered keeper {"<<keeper_id_card<<"} current_time="<<ctime(&current_time)<<"} delayedExitTime= " << std::ctime(&delayedExitTime)<<std::endl;

	    (*keeper_process_iter).second.delayedExitClients.push_back(std::pair<std::time_t, DataStoreAdminClient*>
             ( delayedExitTime, (*keeper_process_iter).second.keeperAdminClient) );
        (*keeper_process_iter).second.keeperAdminClient = nullptr;
        } 
   
	    /*if( (*keeper_process_iter).second.keeperAdminClient != nullptr)
	    {  delete (*keeper_process_iter).second.keeperAdminClient; }
	    keeperProcessRegistry.erase(keeper_process_iter);
        */
	}
	// now that we are still holding registryLock
	// update registryState if needed
	if( !is_shutting_down() && (1 == keeperProcessRegistry.size()))
	{ 
        registryState = INITIALIZED;  
	    std::cout<<"KeeperRegistry: state {INITIALIZED}" << " with {"<< keeperProcessRegistry.size()<<"} KeeperProcesses"<<std::endl;
	}

    return CL_SUCCESS;
}
/////////////////

void KeeperRegistry::updateKeeperProcessStats(KeeperStatsMsg const & keeperStatsMsg) 
{
	// NOTE: we don't lock registryLock while updating the keeperProcess stats
    // delayed destruction of keeperProcessEntyr protects us from the case when
    // stats message is received from the KeeperProcess that has unregistered 
    // on the other thread
    if(is_shutting_down())
    {  return; }

	KeeperIdCard keeper_id_card = keeperStatsMsg.getKeeperIdCard();
	auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
	if(keeper_process_iter == keeperProcessRegistry.end() || !((*keeper_process_iter).second.active) )
	{    // however unlikely it is that the stats msg would be delivered for the keeper that's already unregistered
		// we should probably log a warning here...
		return;
	}
   	KeeperProcessEntry keeper_process = (*keeper_process_iter).second;
	keeper_process.lastStatsTime = std::chrono::steady_clock::now().time_since_epoch().count();
	keeper_process.activeStoryCount = keeperStatsMsg.getActiveStoryCount();

}
/////////////////

std::vector<KeeperIdCard> & KeeperRegistry::getActiveKeepers( std::vector<KeeperIdCard> & keeper_id_cards) 
{  
       //the process of keeper selection will probably get more nuanced; 
	   //for now just return all active keepers

           if(is_shutting_down())
           {  return keeper_id_cards;}

	   std::lock_guard<std::mutex> lock(registryLock);
           if(is_shutting_down())
           {  return keeper_id_cards;}

	   keeper_id_cards.clear();
        std::time_t current_time = std::chrono::high_resolution_clock::to_time_t(std::chrono::high_resolution_clock::now());

	   for (auto iter = keeperProcessRegistry.begin(); iter != keeperProcessRegistry.end(); )
        {
            // first check if there are any delayedExit DataStoreClients to be deleted
            while( !(*iter).second.delayedExitClients.empty() 
              && ( current_time >= (*iter).second.delayedExitClients.front().first ))
            {
                auto dataStoreClientPair = (*iter).second.delayedExitClients.front();
                std::cout <<"INFO: KeeperRegistry::getActiveKeepers destroys old client for {"<<(*iter).second.idCard<<"} current_time="<<ctime(&current_time) <<" delayedTime=" << ctime(&(dataStoreClientPair.first))<<std::endl;
                if(dataStoreClientPair.second != nullptr)
                {  delete dataStoreClientPair.second; }
                (*iter).second.delayedExitClients.pop_front();
            }

            if((*iter).second.active)
		    {
                // active keeper process is ready for story recording    
                keeper_id_cards.push_back((*iter).second.idCard); 
                ++iter;
            }
            else if( (*iter).second.delayedExitClients.empty()) 
            {
                // it's safe to erase the entry for unregistered keeper process
                std::cout <<"INFO: KeeperRegistry::getActiveKeepers destroys keeperProcessEntry for {"<<(*iter).second.idCard<<"} current_time="<<ctime(&current_time)<<std::endl;

	            if( (*iter).second.keeperAdminClient != nullptr)  //this should not be the case so may be removed from here
	            {  delete (*iter).second.keeperAdminClient; }
	            iter = keeperProcessRegistry.erase(iter);
                
            }
            else    
            {
                std::cout <<"DEBUG: KeeperRegistry::getActiveKeepers still keeps keeperProcessEntry for {"<<(*iter).second.idCard<<"} current_time="<<ctime(&current_time)<<"} delayedExitTime= " << std::ctime(&((*iter).second.delayedExitTime))<<std::endl;

                ++iter; 
            }
        }

	   return keeper_id_cards;
}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStart( std::vector<KeeperIdCard> & vectorOfKeepers
		, ChronicleName const& chronicle, StoryName const& story, StoryId const& storyId)
{ 
   if (!is_running())
   {
      std::cout<<"Registry has no Keeper processes to start story recording" << std::endl;
      return CL_ERR_NO_KEEPERS;
   }
   
   std::chrono::time_point<std::chrono::system_clock> time_now = std::chrono::system_clock::now();
   uint64_t story_start_time = time_now.time_since_epoch().count() ;
    
    std::vector<KeeperIdCard> vectorOfKeepersToNotify = vectorOfKeepers;
    vectorOfKeepers.clear();
    for ( KeeperIdCard keeper_id_card : vectorOfKeepersToNotify)
    {
        DataStoreAdminClient * dataAdminClient = nullptr;
	    std::cout << "Registry::notifyStart : story {"<<storyId<<"} BEFORE {"<<story_start_time<<"}"<<std::endl;
        {
        // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
        // We delay the destruction of unactive keeperProcessEntries that might be triggered by the keeper unregister call from a different thread 
        // (see unregisterKeeperProcess()) to protect us from the unfortunate case of keeperProcessEntry.dataAdminClient object being deleted 
        // while this thread is waiting for rpc response
            std::lock_guard<std::mutex> lock(registryLock);
            auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
            if( (keeper_process_iter != keeperProcessRegistry.end()
                && (*keeper_process_iter).second.active) && ((*keeper_process_iter).second.keeperAdminClient != nullptr))
            {
                dataAdminClient = (*keeper_process_iter).second.keeperAdminClient;
            }
            else
            {
	            std::cout<<"WARNING: keeper {"<<keeper_id_card<<"} is not available for notification..."<<std::endl;
                continue;
            }
        }

	    try
	    {
	        int rpc_return = dataAdminClient->send_start_story_recording(chronicle, story, storyId, story_start_time);
	        if (rpc_return != CL_SUCCESS)
	        {
	            std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"}"<<std::endl;
	        }
            else
            {
	            std::cout << "Registry notified keeper {"<<keeper_id_card<<"} to start recording story {"<<storyId<<"} start_time {"<<story_start_time<<"} "<<std::endl;
	            vectorOfKeepers.push_back(keeper_id_card); 
            }
	    }
	    catch(thallium::exception const& ex)
	    {
	        std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"} details: "<<std::endl;
	    }
	    std::cout << "Registry::notifyStart : story {"<<storyId<<"} AFTER {"<<story_start_time<<"}"<<std::endl;
    }

    if ( vectorOfKeepers.empty())
    {
	  std::cout<<"ERROR: notifyKeepersStart:Registry failed to notify the keepers to start recording story {"<<storyId<<"}"<<std::endl;
	  return CL_ERR_NO_KEEPERS;
    }

	std::cout<<"notifyKeepersStart: Registry notified the keepers to start recording story {"<<storyId<<"}"<<std::endl;
    return CL_SUCCESS;
}
/////////////////

int KeeperRegistry::notifyKeepersOfStoryRecordingStop(std::vector<KeeperIdCard> & vectorOfKeepers, StoryId const& storyId)
{
    if (!is_running())
    {
       std::cout<<"Registry has no Keeper processes to notify of story release" << std::endl;
       return CL_ERR_NO_KEEPERS;
    }
    
    size_t keepers_left_to_notify = vectorOfKeepers.size();
    for (KeeperIdCard keeper_id_card : vectorOfKeepers)
    {
        DataStoreAdminClient * dataAdminClient = nullptr;
	    std::cout << "Registry::notifyStart : story {"<<storyId<<"} BEFORE {"<<"}"<<std::endl;
        {
        // NOTE: we release the registryLock before sending rpc request so that we do not hold it for the duration of rpc communication.
        // We delay the destruction of unactive keeperProcessEntries that might be triggered by the keeper unregister call from a different thread 
        // (see unregisterKeeperProcess()) to protect us from the unfortunate case of keeperProcessEntry.dataAdminClient object being deleted 
        // while this thread is waiting for rpc response
            std::lock_guard<std::mutex> lock(registryLock);
            auto keeper_process_iter = keeperProcessRegistry.find(std::pair<uint32_t,uint16_t>(keeper_id_card.getIPaddr(),keeper_id_card.getPort()));
            if( (keeper_process_iter != keeperProcessRegistry.end()
                && (*keeper_process_iter).second.active) && ((*keeper_process_iter).second.keeperAdminClient != nullptr))
            {
                dataAdminClient = (*keeper_process_iter).second.keeperAdminClient;
            }
            else
            {
	            std::cout<<"WARNING: keeper {"<<keeper_id_card<<"} is not available for notification..."<<std::endl;
                continue;
            }
        }
	    try
	    {
	        int rpc_return = dataAdminClient->send_stop_story_recording(storyId);
	        if (rpc_return != CL_SUCCESS)
            {
	            std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"}"<<std::endl;
	        }
	        else
            {
                std::cout << "Registry notified keeper {"<<keeper_id_card<<"} to stop recording story {"<<storyId<<"}"<<std::endl;
            }
	    }
	    catch(thallium::exception const& ex)
	    {
	        std::cout<<"WARNING: Registry failed notification RPC to keeper {"<<keeper_id_card<<"} details: "<<std::endl;
	    }
	    std::cout << "Registry::notifyStop : story {"<<storyId<<"} AFTER {}"<<std::endl;
    }

    return CL_SUCCESS;
}


}//namespace chronolog


