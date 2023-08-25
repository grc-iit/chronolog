
#include <unistd.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <signal.h>

#include "chrono_common/KeeperIdCard.h"
#include "chrono_common/KeeperStatsMsg.h"
#include "KeeperRecordingService.h"
#include "KeeperRegClient.h"
#include "IngestionQueue.h"
#include "StoryChunkExtractionQueue.h"
#include "StoryChunkExtractor.h"
#include "KeeperDataStore.h"
#include "DataStoreAdminService.h"
#include "ConfigurationManager.h"
#include "StoryChunkExtractor.h"
#include "CSVFileChunkExtractor.h"
#include "cmd_arg_parse.h"

#define KEEPER_GROUP_ID 7

// we will be using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number
int service_endpoint_from_dotted_string(std::string const &ip_string, int port, std::pair<uint32_t, uint16_t> &endpoint)
{
    // we will be using a combination of the uint32_t representation of the service IP address
    // and uint16_t representation of the port number
    // NOTE: both IP and port values in the KeeperCard are in the host byte order, not the network order)
    // to identfy the ChronoKeeper process

    struct sockaddr_in sa;
    // translate the recording service dotted IP string into 32bit network byte order representation
    int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if (1 != inet_pton_return)
    {
        std::cout << "invalid ip address" << std::endl;
        return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr);
    uint16_t ntoh_port = port;
    endpoint = std::pair<uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    return 1;
}

volatile sig_atomic_t keep_running = true;

void sigterm_handler (int)
{
    std::cout << "Received SIGTERM, starrt shutting down "<<std::endl;

    keep_running = false;
    return;
}


///////////////////////////////////////////////

int main(int argc, char **argv)
{

    int exit_code = 0;

    signal(SIGTERM, sigterm_handler);


    std::string default_conf_file_path = "./default_conf.json";
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if (conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }

    ChronoLog::ConfigurationManager confManager(conf_file_path);
    // Instantiate ChronoKeeper MemoryDataStore
    //

    // instantiate DataStoreAdminService
    uint64_t keeper_group_id = KEEPER_GROUP_ID;

    std::string datastore_service_ip = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.IP;
    int datastore_service_port = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.BASE_PORT;
    std::string KEEPER_DATASTORE_SERVICE_NA_STRING =
            confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF
            + "://" + datastore_service_ip
            + ":" + std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id =
            confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in KeeperRegistrationMsg

    if (-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
    	std::cout<<"invalid DataStoreAdmin service address"<<std::endl;
	    return (-1);
    }

    // Instantiate KeeperRecordingService

    std::string KEEPER_RECORDING_SERVICE_PROTOCOL = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF;
    std::string KEEPER_RECORDING_SERVICE_IP = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP;
    uint16_t KEEPER_RECORDING_SERVICE_PORT = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

    std::string KEEPER_RECORDING_SERVICE_NA_STRING = std::string(KEEPER_RECORDING_SERVICE_PROTOCOL)
                                                     + "://" + std::string(KEEPER_RECORDING_SERVICE_IP)
                                                     + ":" + std::to_string(KEEPER_RECORDING_SERVICE_PORT);

    // validate ip address, instantiate Recording Service and create KeeperIdCard

    chronolog::service_endpoint recording_endpoint;
    if (-1 == service_endpoint_from_dotted_string(KEEPER_RECORDING_SERVICE_IP, KEEPER_RECORDING_SERVICE_PORT,
                                                  recording_endpoint))
    {
    	std::cout<<"invalid KeeperRecordingService  address"<<std::endl;
	    return (-1);
    }

    // create KeeperIdCard to identify this Keeper process in ChronoVisor's KeeperRegistry
    chronolog::KeeperIdCard keeperIdCard( keeper_group_id, recording_endpoint.first, recording_endpoint.second, recording_service_provider_id);
    std::cout << keeperIdCard<<std::endl;

    // Instantiate ChronoKeeper MemoryDataStore & ExtractorModule
    chronolog::IngestionQueue ingestionQueue;
    std::string keeper_csv_files_directory= confManager.KEEPER_CONF.STORY_FILES_DIR;
    chronolog::CSVFileStoryChunkExtractor storyExtractor( keeperIdCard, keeper_csv_files_directory);
    chronolog::KeeperDataStore theDataStore(ingestionQueue, storyExtractor.getExtractionQueue());

    chronolog::ServiceId collectionServiceId(datastore_endpoint.first, datastore_endpoint.second,
                                             datastore_service_provider_id);
    tl::engine * dataAdminEngine = nullptr;

    chronolog::DataStoreAdminService * keeperDataAdminService = nullptr;

    try
    {
        margo_instance_id collection_margo_id=margo_init( KEEPER_DATASTORE_SERVICE_NA_STRING.c_str(),MARGO_SERVER_MODE, 1, 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        std::cout << "ChronoKeeperInstance group_id {" << keeper_group_id << "} starting DataStoreAdminService at address {"
              << dataAdminEngine->self()
              << "} with provider_id {" << datastore_service_provider_id << "}" << std::endl;

        keeperDataAdminService =
            chronolog::DataStoreAdminService::CreateDataStoreAdminService(*dataAdminEngine,
                                                                          datastore_service_provider_id, theDataStore);
    }
    catch(tl::exception const&)
    {
        std::cout <<"ERROR: Keeper failed to create DataStoreAdminService; "<<std::endl;
    }

    if(nullptr == keeperDataAdminService)
    {
        std::cout <<"ERROR: Keeper failed to create DataStoreAdminService; exiting"<<std::endl;
        if(dataAdminEngine)
        { delete dataAdminEngine; }
        return(-1);
    }

    // Instantiate KeeperRecordingService 
    tl::engine * recordingEngine = nullptr;
    chronolog::KeeperRecordingService *keeperRecordingService = nullptr;

    try
    {
        margo_instance_id margo_id=margo_init( KEEPER_RECORDING_SERVICE_NA_STRING.c_str(),MARGO_SERVER_MODE, 1, 1);

        recordingEngine= new tl::engine(margo_id);

        std::cout << "ChronoKeeperInstance group_id {" << keeper_group_id
              << "} starting KeeperRecordingService at address {" << recordingEngine->self()
              << "} with provider_id {" << recording_service_provider_id << "}" << std::endl;

        keeperRecordingService =
            chronolog::KeeperRecordingService::CreateKeeperRecordingService(*recordingEngine,
                                                                            recording_service_provider_id,
                                                                            ingestionQueue);
    }
    catch(tl::exception const& )
    {
        std::cout <<"ERROR: Keeper failed to create KeeperRecordingService; "<<std::endl;
    }

    if(nullptr == keeperRecordingService)
    {
        std::cout <<"ERROR: Keeper failed to create KeeperRecordingService; exiting"<<std::endl;
        delete keeperDataAdminService;
        return (-1); 
    }

    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry
    std::string KEEPER_REGISTRY_SERVICE_NA_STRING =
            confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF
            + "://" + confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP
            + ":" + std::to_string(confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT);

    uint16_t KEEPER_REGISTRY_SERVICE_PROVIDER_ID = confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

    chronolog::KeeperRegistryClient *keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
            *dataAdminEngine, KEEPER_REGISTRY_SERVICE_NA_STRING, KEEPER_REGISTRY_SERVICE_PROVIDER_ID);
    if(nullptr == keeperRegistryClient)
    {
        std::cout <<"ERROR: Keeper failed to create KeeperRegistryClient; exiting"<<std::endl;
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1); 
    }

    //try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = CL_ERR_UNKNOWN;
    int retries =5;
    while( (CL_SUCCESS != registration_status) && (retries>0))
    {
        registration_status = keeperRegistryClient->send_register_msg(chronolog::KeeperRegistrationMsg(keeperIdCard, collectionServiceId));
        retries--;
    }
    
    if(CL_SUCCESS != registration_status)
    {
        std::cout <<"ERROR: Keeper failed to register with the ChronoVisor; exiting"<<std::endl;
        delete keeperRegistryClient;
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1); 
    }

    // services are successfulley created and keeper process had registered with ChronoVisor
    // start all dataColelction and Extraction threads...
    tl::abt scope;

    theDataStore.startDataCollection(3);

    // start extraction streams & threads
    storyExtractor.startExtractionThreads(2);

   // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    while( keep_running)
    {
        keeperRegistryClient->send_stats_msg(keeperStatsMsg);
        sleep(30);
    }

    //unregister from the chronoVisor so that no new story requests would be coming
    keeperRegistryClient->send_unregister_msg(keeperIdCard);
    delete keeperRegistryClient;

    //stop recording events
    delete keeperRecordingService;
    delete keeperDataAdminService;

    //shutdown the Data Collection
    theDataStore.shutdownDataCollection();

    // shutdown extraction module
    // drain extractionQueue and stop extraction xStreams
    storyExtractor.shutdownExtractionThreads();

    // these are not probably needed as thalium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();

    delete recordingEngine;
    delete dataAdminEngine;

    return exit_code;
}

