
#include <arpa/inet.h>

//#include "Storyteller.h"


#include "KeeperIdCard.h"
#include "KeeperStatsMsg.h"
#include "KeeperRecordingService.h"
#include "KeeperRegClient.h"
#include "LogIngestionQueue.h"

#define KEEPER_REGISTRY_SERVICE_NA_STRING  "ofi+sockets://127.0.0.1:1234"
#define KEEPER_REGISTRY_SERVICE_PROVIDER_ID	25

#define KEEPER_RECORDING_SERVICE_PROTOCOL  "ofi+sockets"
#define KEEPER_RECORDING_SERVICE_IP  "127.0.0.1"
#define KEEPER_RECORDING_SERVICE_PORT  "5555"
#define KEEPER_RECORDING_SERVICE_PROVIDER_ID	22
#define IP_ADDRESS "127.0.0.1"

int main(int argc, char** argv) {

  int exit_code = 0;

    std::string KEEPER_RECORDING_SERVICE_NA_STRING = std::string(KEEPER_RECORDING_SERVICE_PROTOCOL)
	                                           +"://"+std::string(KEEPER_RECORDING_SERVICE_IP)
						   +":"+std::string(KEEPER_RECORDING_SERVICE_PORT);
    uint16_t recording_provider_id = KEEPER_RECORDING_SERVICE_PROVIDER_ID;

    margo_instance_id margo_id=margo_init( KEEPER_RECORDING_SERVICE_NA_STRING.c_str(),MARGO_SERVER_MODE, 1, 0);

    if(MARGO_INSTANCE_NULL == margo_id)
    {
      std::cout<<"FAiled to initialise margo_instance"<<std::endl;
      return 1;
    }
    std::cout<<"margo_instance initialized"<<std::endl;

   tl::engine keeperEngine(margo_id);
 
    std::cout << "Starting KeeperRecordingService  at address " << keeperEngine.self()
        << " with provider id " << recording_provider_id << std::endl;


    // Instantiate ChronoKeeper MemoryDataStore
    //
    chronolog::LogIngestionQueue ingestionQueue; 
    // Instantiate KeeperRecordingService 

   chronolog::KeeperRecordingService *  keeperRecordingService=
	   chronolog::KeeperRecordingService::CreateKeeperRecordingService(keeperEngine, recording_provider_id, ingestionQueue);


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
    
    chronolog::KeeperIdCard keeperIdCard( ntoh_ip_addr, ntoh_port, recording_provider_id);
    std::cout << keeperIdCard<<std::endl;

    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry 
    chronolog::KeeperRegistryClient * keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
		     keeperEngine, KEEPER_REGISTRY_SERVICE_NA_STRING, KEEPER_REGISTRY_SERVICE_PROVIDER_ID);

    keeperRegistryClient->send_register_msg(keeperIdCard);
    keeperRegistryClient->send_unregister_msg(keeperIdCard);

    // now we are ready to ingest records coming from the storyteller clients ....

    // for now BOTH KeeperRecordingService and KeeperRegistryClient will be running until they are explicitly killed  
    // INNA: TODO: add a graceful shutdown mechanism with finalized callbacks and all
    //
    keeperEngine.wait_for_finalize();
   // delete keeperRegistryClient;
   // delete keeperRecordingService;


return exit_code;
}
