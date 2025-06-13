#include <unistd.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <signal.h>

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
#include "StoryChunkExtractorRDMA.h"

// we will be using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number
int
service_endpoint_from_dotted_string(std::string const &ip_string, int port, std::pair <uint32_t, uint16_t> &endpoint)
{
    // we will be using a combination of the uint32_t representation of the service IP address
    // and uint16_t representation of the port number
    // NOTE: both IP and port values in the KeeperCard are in the host byte order, not the network order)
    // to identify the ChronoKeeper process

    struct sockaddr_in sa;
    // translate the recording service dotted IP string into 32bit network byte order representation
    int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if(1 != inet_pton_return)
    {
        LOG_ERROR("[ChronoKeeperInstance] Invalid IP address provided: {}", ip_string);
        return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr);
    uint16_t ntoh_port = port;
    endpoint = std::pair <uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    LOG_DEBUG("[ChronoKeeperInstance] Service endpoint created: IP={}, Port={}", ip_string, port);
    return 1;
}

volatile sig_atomic_t keep_running = true;

void sigterm_handler(int)
{
    LOG_INFO("[ChronoKeeperInstance] Received SIGTERM signal. Initiating shutdown procedure.");
    keep_running = false;
    return;
}

///////////////////////////////////////////////

int main(int argc, char**argv)
{
    int exit_code = 0;
    signal(SIGTERM, sigterm_handler);

    /// Configure SetUp ________________________________________________________________________________________________
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    ChronoLog::ConfigurationManager confManager(conf_file_path);
    int result = chronolog::chrono_monitor::initialize(confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGTYPE
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILE
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGLEVEL
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGNAME
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILESIZE
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.LOGFILENUM
                                                       , confManager.KEEPER_CONF.KEEPER_LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("Running ChronoKeeper Server.");

    // Instantiate ChronoKeeper MemoryDataStore
    // instantiate DataStoreAdminService

    /// DataStoreAdminService setup ____________________________________________________________________________________
    std::string datastore_service_ip = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.IP;
    int datastore_service_port = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.BASE_PORT;
    std::string KEEPER_DATASTORE_SERVICE_NA_STRING =
            confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
            datastore_service_ip + ":" + std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id = confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in KeeperRegistrationMsg

    if(-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to start DataStoreAdminService. Invalid endpoint provided.");
        return (-1);
    }

    chronolog::ServiceId dataStoreServiceId( confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.PROTO_CONF,
                        datastore_endpoint, confManager.KEEPER_CONF.KEEPER_DATA_STORE_ADMIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID);

 
    /// KeeperRecordingService setup ___________________________________________________________________________________
    // Instantiate KeeperRecordingService
    std::string KEEPER_RECORDING_SERVICE_PROTOCOL = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF;
    std::string KEEPER_RECORDING_SERVICE_IP = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP;
    uint16_t KEEPER_RECORDING_SERVICE_PORT = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;


    // validate ip address, instantiate Recording Service and create KeeperIdCard

    chronolog::service_endpoint recording_endpoint;
    if(-1 == service_endpoint_from_dotted_string(KEEPER_RECORDING_SERVICE_IP, KEEPER_RECORDING_SERVICE_PORT
                                                 , recording_endpoint))
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to start KeeperRecordingService. Invalid endpoint provided.");
        return (-1);
    }
    LOG_INFO("[ChronoKeeperInstance] KeeperRecordingService started successfully.");

    // create KeeperIdCard to identify this Keeper process in ChronoVisor's KeeperRegistry
    chronolog::RecordingGroupId keeper_group_id = confManager.KEEPER_CONF.RECORDING_GROUP;
    chronolog::KeeperIdCard keeperIdCard(keeper_group_id, 
            chronolog::ServiceId(confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF,
            confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP,
            confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT,
            confManager.KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID));

    std::string KEEPER_RECORDING_SERVICE_NA_STRING;
    keeperIdCard.getRecordingServiceId().get_service_as_string(KEEPER_RECORDING_SERVICE_NA_STRING);

    LOG_INFO("[ChronoKeeperInstance] KeeperIdCard: {}", chronolog::to_string(keeperIdCard));

    // Instantiate ChronoKeeper MemoryDataStore & ExtractorModule
    chronolog::IngestionQueue ingestionQueue;
    std::string keeper_csv_files_directory = confManager.KEEPER_CONF.EXTRACTOR_CONF.story_files_dir;
    // Instantiate KeeperGrapherDrainService
    tl::engine*extractionEngine = nullptr;
    tl::remote_procedure drain_to_grapher;
    tl::provider_handle service_ph;
    uint16_t extraction_provider_id = confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;
    try
    {
        extractionEngine = new tl::engine(confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF
                                          , THALLIUM_CLIENT_MODE);

        std::stringstream s1;
        s1 << extractionEngine->self();
        LOG_INFO("[ChronoKeeperInstance] GroupID={} starting extraction engine at address {}", keeper_group_id
                 , s1.str());
        std::string KEEPER_GRAPHER_NA_STRING =
                confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
                confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP + ":" +
                std::to_string(confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT);
        std::string extraction_rpc_name = "record_story_chunk";
        drain_to_grapher = extractionEngine->define(extraction_rpc_name);
        LOG_DEBUG("[ChronoKeeperInstance] Looking up {} at: {} ...", extraction_rpc_name, KEEPER_GRAPHER_NA_STRING);
        service_ph = tl::provider_handle(extractionEngine->lookup(KEEPER_GRAPHER_NA_STRING), extraction_provider_id);
        if(service_ph.is_null())
        {
            LOG_ERROR("[ChronoKeeperInstance] Failed to lookup Grapher service provider handle");
            throw std::runtime_error("Failed to lookup Grapher service provider handle");
        }
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create extraction engine");
    }

    chronolog::StoryChunkExtractorRDMA storyExtractor = chronolog::StoryChunkExtractorRDMA(*extractionEngine
                                                                                           , drain_to_grapher
                                                                                           , service_ph);
    //    chronolog::CSVFileStoryChunkExtractor storyExtractor(keeperIdCard, keeper_csv_files_directory);
    chronolog::KeeperDataStore theDataStore(ingestionQueue, storyExtractor.getExtractionQueue());

    // Instantiate KeeperRecordingService
    tl::engine*dataAdminEngine = nullptr;

    chronolog::DataStoreAdminService*keeperDataAdminService = nullptr;

    try
    {
        margo_instance_id collection_margo_id = margo_init(KEEPER_DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE
                                                           , 1, 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        std::stringstream s3;
        s3 << dataAdminEngine->self();
        LOG_DEBUG("[ChronoKeeperInstance] GroupID={} starting DataStoreAdminService at address {} with ProviderID={}"
                  , keeper_group_id, s3.str(), datastore_service_provider_id);
        keeperDataAdminService = chronolog::DataStoreAdminService::CreateDataStoreAdminService(*dataAdminEngine
                                                                                               , datastore_service_provider_id
                                                                                               , theDataStore);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create DataStoreAdminService");
    }

    LOG_INFO("[ChronoKeeperInstance] DataStoreAdminService started successfully.");

    if(nullptr == keeperDataAdminService)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create DataStoreAdminService exiting");
        if(dataAdminEngine)
        { delete dataAdminEngine; }
        return (-1);
    }

    // Instantiate KeeperRecordingService
    tl::engine*recordingEngine = nullptr;
    chronolog::KeeperRecordingService*keeperRecordingService = nullptr;

    try
    {
        margo_instance_id margo_id = margo_init(KEEPER_RECORDING_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        recordingEngine = new tl::engine(margo_id);

        std::stringstream s1;
        s1 << recordingEngine->self();
        LOG_INFO("[ChronoKeeperInstance] GroupID={} starting KeeperRecordingService at {} with provider_id {}"
                 , keeper_group_id, s1.str(), recording_service_provider_id);
        keeperRecordingService = chronolog::KeeperRecordingService::CreateKeeperRecordingService(*recordingEngine
                                                                                                 , recording_service_provider_id
                                                                                                 , ingestionQueue);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService");
    }

    if(nullptr == keeperRecordingService)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService exiting");
        delete keeperDataAdminService;
        return (-1);
    }

    /// KeeperRegistryClient SetUp _____________________________________________________________________________________
    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry
    std::string KEEPER_REGISTRY_SERVICE_NA_STRING =
            confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
            confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.IP + ":" +
            std::to_string(confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.BASE_PORT);

    uint16_t KEEPER_REGISTRY_SERVICE_PROVIDER_ID = confManager.KEEPER_CONF.VISOR_KEEPER_REGISTRY_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;

    chronolog::KeeperRegistryClient*keeperRegistryClient = chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(
            *dataAdminEngine, KEEPER_REGISTRY_SERVICE_NA_STRING, KEEPER_REGISTRY_SERVICE_PROVIDER_ID);

    if(nullptr == keeperRegistryClient)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create KeeperRegistryClient; exiting");
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1);
    }

    /// Registration with ChronoVisor __________________________________________________________________________________
    // try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = chronolog::CL_ERR_UNKNOWN;
    int retries = 5;
    while((chronolog::CL_SUCCESS != registration_status) && (retries > 0))
    {
        registration_status = keeperRegistryClient->send_register_msg(
                chronolog::KeeperRegistrationMsg(keeperIdCard, dataStoreServiceId));
        retries--;
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to register with ChronoVisor after multiple attempts. Exiting.");
        delete keeperRegistryClient;
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1);
    }
    LOG_INFO("[ChronoKeeperInstance] Successfully registered with ChronoVisor.");

    /// Start data collection and extraction threads ___________________________________________________________________
    // services are successfully created and keeper process had registered with ChronoVisor
    // start all dataCollection and Extraction threads...
    tl::abt scope;
    theDataStore.startDataCollection(3);
    // start extraction streams & threads
    storyExtractor.startExtractionThreads(2);


    /// Main loop for sending stats message until receiving SIGTERM ____________________________________________________
    // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    while(keep_running)
    {
        keeperRegistryClient->send_stats_msg(keeperStatsMsg);
        sleep(10);
    }

    /// Unregister from ChronoVisor ____________________________________________________________________________________
    // Unregister from the chronoVisor so that no new story requests would be coming
    keeperRegistryClient->send_unregister_msg(keeperIdCard);
    delete keeperRegistryClient;

    /// Stop services and shut down ____________________________________________________________________________________
    LOG_INFO("[ChronoKeeperInstance] Initiating shutdown procedures.");
    // Stop recording events
    delete keeperRecordingService;
    delete keeperDataAdminService;
    // Shutdown the Data Collection
    theDataStore.shutdownDataCollection();
    // Shutdown extraction module
    // drain extractionQueue and stop extraction xStreams
    storyExtractor.shutdownExtractionThreads();
    // these are not probably needed as thallium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();
    delete extractionEngine;
    delete recordingEngine;
    delete dataAdminEngine;
    LOG_INFO("[ChronoKeeperInstance] Shutdown completed. Exiting.");
    return exit_code;
}
