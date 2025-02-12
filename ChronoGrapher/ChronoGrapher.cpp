#include <unistd.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <signal.h>

#include "GrapherIdCard.h"
#include "GrapherRecordingService.h"
#include "GrapherRegClient.h"
#include "ChunkIngestionQueue.h"
#include "StoryChunkExtractionQueue.h"
#include "StoryChunkExtractor.h"
#include "GrapherDataStore.h"
#include "DataStoreAdminService.h"
#include "ConfigurationManager.h"
#include "CSVFileChunkExtractor.h"
#include "HDF5FileChunkExtractor.h"
#include "cmd_arg_parse.h"

namespace chl = chronolog;

// we will be using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number
int
service_endpoint_from_dotted_string(std::string const &ip_string, int port, std::pair <uint32_t, uint16_t> &endpoint)
{
    // we will be using a combination of the uint32_t representation of the service IP address
    // and uint16_t representation of the port number
    // NOTE: both IP and port values in the IdCard are in the host byte order, not the network order)
    // to identify the Chrono process

    struct sockaddr_in sa;
    // translate the recording service dotted IP string into 32bit network byte order representation
    int inet_pton_return = inet_pton(AF_INET, ip_string.c_str(), &sa.sin_addr.s_addr); //returns 1 on success
    if(1 != inet_pton_return)
    {
        LOG_ERROR("[ChronoGrapher] Invalid IP address provided: {}", ip_string);
        return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr);
    uint16_t ntoh_port = port;
    endpoint = std::pair <uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    LOG_DEBUG("[ChronoGrapher] Service endpoint created: IP={}, Port={}", ip_string, port);
    return 1;
}

volatile sig_atomic_t keep_running = true;

void sigterm_handler(int)
{
    LOG_INFO("[ChronoGrapher] Received SIGTERM signal. Initiating shutdown procedure.");
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
    int result = chronolog::chrono_monitor::initialize(confManager.GRAPHER_CONF.LOG_CONF.LOGTYPE
                                                       , confManager.GRAPHER_CONF.LOG_CONF.LOGFILE
                                                       , confManager.GRAPHER_CONF.LOG_CONF.LOGLEVEL
                                                       , confManager.GRAPHER_CONF.LOG_CONF.LOGNAME
                                                       , confManager.GRAPHER_CONF.LOG_CONF.LOGFILESIZE
                                                       , confManager.GRAPHER_CONF.LOG_CONF.LOGFILENUM
                                                       , confManager.GRAPHER_CONF.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("Running ChronoGrapher ");

    // Instantiate MemoryDataStore
    // instantiate DataStoreAdminService

    /// DataStoreAdminService setup ____________________________________________________________________________________
    std::string datastore_service_ip = confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP;
    int datastore_service_port = confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT;
    std::string DATASTORE_SERVICE_NA_STRING =
            confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF + "://" + datastore_service_ip + ":" +
            std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id = confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in RegistrationMsg

    if(-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
        LOG_CRITICAL("[ChronoGrapher] Failed to start DataStoreAdminService. Invalid endpoint provided.");
        return (-1);
    }
    
    chronolog::ServiceId dataStoreServiceId(confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF,
                datastore_endpoint, confManager.GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID);

    // Instantiate GrapherRecordingService
    chronolog::RecordingGroupId recording_group_id = confManager.GRAPHER_CONF.RECORDING_GROUP;
    std::string RECORDING_SERVICE_PROTOCOL = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF;
    std::string RECORDING_SERVICE_IP = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP;
    uint16_t RECORDING_SERVICE_PORT = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID;
    
    chl::ServiceId recordingServiceId(confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF,
                confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP,
                confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT,
                confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID);


    std::string RECORDING_SERVICE_NA_STRING;
    recordingServiceId.get_service_as_string(RECORDING_SERVICE_NA_STRING);
    
    // validate ip address, instantiate Recording Service and create IdCard

    chronolog::service_endpoint recording_endpoint;
    if(-1 == service_endpoint_from_dotted_string(RECORDING_SERVICE_IP, RECORDING_SERVICE_PORT, recording_endpoint))
    {
        LOG_CRITICAL("[ChronoGrapher] Failed to start RecordingService. Invalid endpoint provided.");
        return (-1);
    }
    LOG_INFO("[ChronoGrapher] RecordingService started successfully.");

    // create GrapherIdCard to identify this Grapher process in ChronoVisor's Registry
    chronolog::GrapherIdCard processIdCard(recording_group_id, recordingServiceId);

    LOG_INFO("[ChronoGrapher] GrapherIdCard: {}", chl::to_string(processIdCard));

    // Instantiate MemoryDataStore & ExtractorModule
    chronolog::ChunkIngestionQueue ingestionQueue;
    std::string csv_files_directory = confManager.GRAPHER_CONF.EXTRACTOR_CONF.story_files_dir;

//    chronolog::CSVFileStoryChunkExtractor storyExtractor(process_id_string.str(), csv_files_directory);
    chronolog::HDF5FileChunkExtractor storyExtractor(chl::to_string(processIdCard), csv_files_directory);
    chronolog::GrapherDataStore theDataStore(ingestionQueue, storyExtractor.getExtractionQueue());

    tl::engine*dataAdminEngine = nullptr;

    chronolog::DataStoreAdminService*grapherDataAdminService = nullptr;

    try
    {
        margo_instance_id collection_margo_id = margo_init(DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1
                                                           , 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        std::stringstream s3;
        s3 << dataAdminEngine->self();
        LOG_DEBUG("[ChronoGrapher] starting DataStoreAdminService at {} ", chl::to_string(dataStoreServiceId));
        grapherDataAdminService = chronolog::DataStoreAdminService::CreateDataStoreAdminService(*dataAdminEngine
                                                                                                , datastore_service_provider_id
                                                                                                , theDataStore);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoGrapher]  failed to create DataStoreAdminService");
        grapherDataAdminService = nullptr;
    }

    if(nullptr == grapherDataAdminService)
    {
        LOG_CRITICAL("[ChronoGrapher] failed to create DataStoreAdminService exiting");
        return (-1);
    }
    LOG_INFO("[ChronoGrapher] DataStoreAdminService started successfully.");

    // Instantiate RecordingService
    tl::engine*recordingEngine = nullptr;
    chronolog::GrapherRecordingService*grapherRecordingService = nullptr;

    try
    {
        margo_instance_id margo_id = margo_init(RECORDING_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);
        recordingEngine = new tl::engine(margo_id);

        std::stringstream s1;
        s1 << recordingEngine->self();
        LOG_INFO("[ChronoGrapher] starting RecordingService at {} with provider_id {}", s1.str()
                 , recording_service_provider_id);
        grapherRecordingService = chronolog::GrapherRecordingService::CreateRecordingService(*recordingEngine
                                                                                             , recording_service_provider_id
                                                                                             , ingestionQueue);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoGrapher] failed to create RecordingService");
        grapherRecordingService = nullptr;
    }

    if(nullptr == grapherRecordingService)
    {
        LOG_CRITICAL("[ChronoGrapher] failed to create RecordingService exiting");
        delete grapherDataAdminService;
        return (-1);
    }

    /// RegistryClient SetUp _____________________________________________________________________________________
    // create RegistryClient and register the new Recording service with the Registry
    std::string REGISTRY_SERVICE_NA_STRING = confManager.GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF + "://" +
                                             confManager.GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.IP + ":" +
                                             std::to_string(
                                                     confManager.GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.BASE_PORT);

    uint16_t REGISTRY_SERVICE_PROVIDER_ID = confManager.GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::GrapherRegistryClient*grapherRegistryClient = chronolog::GrapherRegistryClient::CreateRegistryClient(
            *dataAdminEngine, REGISTRY_SERVICE_NA_STRING, REGISTRY_SERVICE_PROVIDER_ID);

    if(nullptr == grapherRegistryClient)
    {
        LOG_CRITICAL("[ChronoGrapher] failed to create RegistryClient; exiting");
        delete grapherRecordingService;
        delete grapherDataAdminService;
        return (-1);
    }

    /// Registration with ChronoVisor __________________________________________________________________________________
    // try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = grapherRegistryClient->send_register_msg(
            chronolog::GrapherRegistrationMsg(processIdCard, dataStoreServiceId));
    //if the first attemp failes retry 
    int retries = 5;
    while((chronolog::CL_SUCCESS != registration_status) && (retries > 0))
    {
        registration_status = grapherRegistryClient->send_register_msg(
                chronolog::GrapherRegistrationMsg(processIdCard, dataStoreServiceId));

        sleep(5);
        retries--;
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoGrapher] Failed to register with ChronoVisor after multiple attempts. Exiting.");
        delete grapherRegistryClient;
        delete grapherRecordingService;
        delete grapherDataAdminService;
        return (-1);
    }
    LOG_INFO("[ChronoGrapher] Successfully registered with ChronoVisor.");

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
    chronolog::GrapherStatsMsg grapherStatsMsg(processIdCard);
    while(keep_running)
    {
        grapherRegistryClient->send_stats_msg(grapherStatsMsg);
        sleep(10);
    }

    /// Unregister from ChronoVisor ____________________________________________________________________________________
    // Unregister from the chronoVisor so that no new story requests would be coming
    grapherRegistryClient->send_unregister_msg(processIdCard);
    delete grapherRegistryClient;

    /// Stop services and shut down ____________________________________________________________________________________
    LOG_INFO("[ChronoGrapher] Initiating shutdown procedures.");
    // Stop recording events
    delete grapherRecordingService;
    delete grapherDataAdminService;
    // Shutdown the Data Collection
    theDataStore.shutdownDataCollection();
    // Shutdown extraction module
    // drain extractionQueue and stop extraction xStreams
    storyExtractor.shutdownExtractionThreads();
    // these are not probably needed as thallium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();
    delete recordingEngine;
    delete dataAdminEngine;
    LOG_INFO("[ChronoGrapher] Shutdown completed. Exiting.");
    return exit_code;
}
