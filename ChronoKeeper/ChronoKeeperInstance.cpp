
#include <arpa/inet.h>

//#include "Storyteller.h"


#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRecordingService.h"
#include "KeeperRegClient.h"
#include "IngestionQueue.h"
#include "KeeperDataStore.h"
#include "DataCollectionService.h"

#define KEEPER_GROUP_ID 7
#define KEEPER_REGISTRY_SERVICE_NA_STRING  "ofi+sockets://127.0.0.1:1234"
#define KEEPER_REGISTRY_SERVICE_PROVIDER_ID	25

#define KEEPER_RECORDING_SERVICE_PROTOCOL  "ofi+sockets"
#define KEEPER_RECORDING_SERVICE_IP  "127.0.0.1"
#define KEEPER_RECORDING_SERVICE_PORT  "5555"
#define KEEPER_RECORDING_SERVICE_PROVIDER_ID	22

#define KEEPER_COLLECTION_SERVICE_PROTOCOL  "ofi+sockets"
#define KEEPER_COLLECTION_SERVICE_IP  "127.0.0.1"
#define KEEPER_COLLECTION_SERVICE_PORT  "7777"
#define KEEPER_COLLECTION_SERVICE_PROVIDER_ID	27

    // we will be using a combination of the uint32_t representation of the service IP address 
    // and uint16_t representation of the port number
int service_endpoint_from_dotted_string(std::string const & ip_string, int port, std::pair<uint32_t,uint16_t> & endpoint)
{	
    // we will be using a combination of the uint32_t representation of the service IP address 
    // and uint16_t representation of the port number 
    // NOTE: both IP and port values in the KeeperCard are in the host byte order, not the network order)
    // to identfy the ChronoKeeper process

   struct sockaddr_in sa;
   // translate the recording service dotted IP string into 32bit network byte order representation
   int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if(1 != inet_pton_return)
    { std::cout<< "invalid ip address"<<std::endl;
	    return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr); 
    uint16_t ntoh_port = port;
    endpoint = std::pair<uint32_t,uint16_t>(ntoh_ip_addr, ntoh_port);

return 1;
}	

int main(int argc, char** argv) {

  int exit_code = 0;

  //INNA: TODO: pass the config file path on the command line & load the parameters inot a ConfigurationObject
  // for now all the arguments are hardcoded ...
    
    uint64_t keeper_group_id = KEEPER_GROUP_ID;

    std::string KEEPER_RECORDING_SERVICE_NA_STRING = std::string(KEEPER_RECORDING_SERVICE_PROTOCOL)
	                                           +"://"+std::string(KEEPER_RECORDING_SERVICE_IP)
						   +":"+std::string(KEEPER_RECORDING_SERVICE_PORT);
    uint16_t recording_provider_id = KEEPER_RECORDING_SERVICE_PROVIDER_ID;

    margo_instance_id margo_id=margo_init( KEEPER_RECORDING_SERVICE_NA_STRING.c_str(),MARGO_SERVER_MODE, 1, 1);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"margo_instance initialized"<<std::endl;

    tl::engine recordingEngine(margo_id);
 
    std::cout << "ChronoKeeperInstance group_id {"<<keeper_group_id<<"} starting KeeperRecordingService at address {" << recordingEngine.self()
        << "} with provider_id {" << recording_provider_id <<"}"<< std::endl;


    // Instantiate ChronoKeeper MemoryDataStore
    //
    chronolog::IngestionQueue ingestionQueue; 
    chronolog::KeeperDataStore theDataStore(ingestionQueue);

    // instantiate DataCollectionService
    std::string KEEPER_COLLECTION_SERVICE_NA_STRING = std::string(KEEPER_COLLECTION_SERVICE_PROTOCOL)
	                                           +"://"+std::string(KEEPER_COLLECTION_SERVICE_IP)
						   +":"+std::string(KEEPER_COLLECTION_SERVICE_PORT);
    uint16_t collection_provider_id = KEEPER_COLLECTION_SERVICE_PROVIDER_ID;

    margo_instance_id collection_margo_id=margo_init( KEEPER_COLLECTION_SERVICE_NA_STRING.c_str(),MARGO_SERVER_MODE, 1, 1);

    if(MARGO_INSTANCE_NULL == collection_margo_id)
    {
      std::cout<<"FAiled to initialise collection_margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"collection_margo_instance initialized"<<std::endl;

   tl::engine collectionEngine(collection_margo_id);
 
    std::cout << "ChronoKeeperInstance group_id {"<<keeper_group_id<<"} starting KeeperCollectionService at address {" << collectionEngine.self()
        << "} with provider_id {" << collection_provider_id <<"}"<< std::endl;
    
    chronolog::DataCollectionService * keeperCollectionService = 
	    chronolog::DataCollectionService::CreateDataCollectionService(collectionEngine,collection_provider_id, theDataStore);

    // Instantiate KeeperRecordingService 

   chronolog::KeeperRecordingService *  keeperRecordingService=
	   chronolog::KeeperRecordingService::CreateKeeperRecordingService(recordingEngine, recording_provider_id, ingestionQueue);


    // we will be using a combination of the uint32_t representation of the service IP address 
    // and uint16_t representation of the port number 
    // NOTE: both IP and port values in the KeeperCard are in the host byte order, not the network order)
    // to identfy the ChronoKeeper process

   struct sockaddr_in sa;
   // translate the recording service dotted IP string into 32bit network byte order representation
   int inet_pton_return = inet_pton(AF_INET, std::string(KEEPER_RECORDING_SERVICE_IP).c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if(1 != inet_pton_return)
    { std::cout<< "invalid ip address"<<std::endl;
	    return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr); 
    uint16_t ntoh_port = atoi(KEEPER_RECORDING_SERVICE_PORT);
    
    chronolog::KeeperIdCard keeperIdCard( keeper_group_id,ntoh_ip_addr, ntoh_port, recording_provider_id);
    std::cout << keeperIdCard<<std::endl;

    chronolog::service_endpoint collection_endpoint;
    int collection_service_port = atoi(KEEPER_COLLECTION_SERVICE_PORT);
    uint16_t collection_service_provider_id = KEEPER_COLLECTION_SERVICE_PROVIDER_ID;

    if( -1 == service_endpoint_from_dotted_string(std::string(KEEPER_COLLECTION_SERVICE_IP), collection_service_port, collection_endpoint) )
    {
    	    std::cout<<"invalid collection service address"<<std::endl;
	    return (-1);
    }	  

    chronolog::ServiceId collectionServiceId(collection_endpoint.first,collection_endpoint.second, collection_service_provider_id);

    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry 
    chronolog::KeeperRegistryClient * keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
		     collectionEngine, KEEPER_REGISTRY_SERVICE_NA_STRING, KEEPER_REGISTRY_SERVICE_PROVIDER_ID);

    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    keeperRegistryClient->send_register_msg(chronolog::KeeperRegistrationMsg(keeperIdCard,collectionServiceId));


    // now we are ready to ingest records coming from the storyteller clients ....

    // for now BOTH KeeperRecordingService and KeeperRegistryClient will be running until they are explicitly killed  
    // INNA: TODO: add a graceful shutdown mechanism with finalized callbacks and all
    //

    int i{0};

    while( !theDataStore.is_shutting_down())
    {
        keeperRegistryClient->send_stats_msg(keeperStatsMsg);
	theDataStore.collectIngestedEvents();
	sleep(10);
    }

    keeperRegistryClient->send_unregister_msg(keeperIdCard);
    delete keeperRegistryClient;
    collectionEngine.wait_for_finalize();
    recordingEngine.wait_for_finalize();
    delete keeperRecordingService;
    delete keeperCollectionService;


return exit_code;
}
