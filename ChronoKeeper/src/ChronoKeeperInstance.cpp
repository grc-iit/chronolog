#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <cstdint>

#include <KeeperRecordingService.h>
#include <KeeperAppendStats.h>
#include <KeeperRegClient.h>
#include <IngestionQueue.h>
#include <StoryChunkExtractionQueue.h>
#include <KeeperDataStore.h>
#include <DataStoreAdminService.h>
#include <cmd_arg_parse.h>
#include <StoryChunkExtractionModule.h>
#include <ChunkLoggingExtractor.h>
#include <ChunkExtractorRDMA.h>

#include <ConfigurationManager.h>
#include <ChronoKeeperConfiguration.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
int env_positive_int(char const* name, int default_value)
{
    char const* value = std::getenv(name);
    if(value == nullptr || *value == '\0')
    {
        return default_value;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if(end == value || parsed <= 0 || parsed > std::numeric_limits<int>::max())
    {
        return default_value;
    }
    return static_cast<int>(parsed);
}

int env_nonnegative_int(char const* name, int default_value)
{
    char const* value = std::getenv(name);
    if(value == nullptr || *value == '\0')
    {
        return default_value;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if(end == value || parsed < 0 || parsed > std::numeric_limits<int>::max())
    {
        return default_value;
    }
    return static_cast<int>(parsed);
}
}

// we will be using a combination of the uint32_t representation of the service IP address
// and uint16_t representation of the port number
int service_endpoint_from_dotted_string(std::string const& ip_string, int port, std::pair<uint32_t, uint16_t>& endpoint)
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
    endpoint = std::pair<uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    LOG_DEBUG("[ChronoKeeperInstance] Service endpoint created: IP={}, Port={}", ip_string, port);
    return 1;
}

volatile sig_atomic_t keep_running = true;
volatile sig_atomic_t append_stats_snapshot_requested = false;

void sigterm_handler(int)
{
    keep_running = false;
    return;
}

void sigusr1_handler(int)
{
    append_stats_snapshot_requested = true;
    return;
}

///////////////////////////////////////////////

int main(int argc, char** argv)
{
    int exit_code = 0;
    signal(SIGTERM, sigterm_handler);
    signal(SIGUSR1, sigusr1_handler);

    /// Configure SetUp ________________________________________________________________________________________________
    std::string conf_file_path;
    conf_file_path = parse_conf_path_arg(argc, argv);
    if(conf_file_path.empty())
    {
        std::exit(EXIT_FAILURE);
    }
    chronolog::ConfigurationManager confManager(conf_file_path);

    chronolog::KeeperConfiguration KEEPER_CONF;
    if(KEEPER_CONF.parseJsonConf(confManager.KEEPER_JSON_CONF) != chronolog::CL_SUCCESS)
    {
        std::cerr << "[ChronoKeeper] Invalid KEEPER configuration. Exiting";
        exit(EXIT_FAILURE);
    }

    int result = chronolog::chrono_monitor::initialize(KEEPER_CONF.LOG_CONF.LOGTYPE,
                                                       KEEPER_CONF.LOG_CONF.LOGFILE,
                                                       KEEPER_CONF.LOG_CONF.LOGLEVEL,
                                                       KEEPER_CONF.LOG_CONF.LOGNAME,
                                                       KEEPER_CONF.LOG_CONF.LOGFILESIZE,
                                                       KEEPER_CONF.LOG_CONF.LOGFILENUM,
                                                       KEEPER_CONF.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    LOG_INFO("[ChronoKeeper] Running ChronoKeeper Server.");
    LOG_INFO("[ChronoKeeper] Configuration {}", KEEPER_CONF.to_String());
    chronolog::chrono_monitor::flush();

    // Instantiate ChronoKeeper MemoryDataStore
    // instantiate DataStoreAdminService

    /// DataStoreAdminService setup ____________________________________________________________________________________
    std::string datastore_service_ip = KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP;
    int datastore_service_port = KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT;
    std::string KEEPER_DATASTORE_SERVICE_NA_STRING = KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF + "://" +
                                                     datastore_service_ip + ":" +
                                                     std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id = KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in KeeperRegistrationMsg

    if(-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to start DataStoreAdminService. Invalid endpoint provided.");
        return (-1);
    }

    chronolog::ServiceId dataStoreServiceId(KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF,
                                            datastore_endpoint,
                                            KEEPER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID);


    /// KeeperRecordingService setup ___________________________________________________________________________________
    // Instantiate KeeperRecordingService
    std::string KEEPER_RECORDING_SERVICE_PROTOCOL = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.PROTO_CONF;
    std::string KEEPER_RECORDING_SERVICE_IP = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.IP;
    uint16_t KEEPER_RECORDING_SERVICE_PORT = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.SERVICE_PROVIDER_ID;


    // validate ip address, instantiate Recording Service and create KeeperIdCard

    chronolog::service_endpoint recording_endpoint;
    if(-1 == service_endpoint_from_dotted_string(KEEPER_RECORDING_SERVICE_IP,
                                                 KEEPER_RECORDING_SERVICE_PORT,
                                                 recording_endpoint))
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to start KeeperRecordingService. Invalid endpoint provided.");
        return (-1);
    }
    LOG_INFO("[ChronoKeeperInstance] KeeperRecordingService started successfully.");

    // create KeeperIdCard to identify this Keeper process in ChronoVisor's KeeperRegistry
    chronolog::RecordingGroupId keeper_group_id = KEEPER_CONF.RECORDING_GROUP;
    chronolog::KeeperIdCard keeperIdCard(
            keeper_group_id,
            chronolog::ServiceId(KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.PROTO_CONF,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.IP,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.BASE_PORT,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.SERVICE_PROVIDER_ID));

    std::string KEEPER_RECORDING_SERVICE_NA_STRING;
    keeperIdCard.getRecordingServiceId().get_service_as_string(KEEPER_RECORDING_SERVICE_NA_STRING);

    LOG_INFO("[ChronoKeeperInstance] KeeperIdCard: {}", chronolog::to_string(keeperIdCard));

    // Instantiate ChronoKeeper MemoryDataStore & ExtractionModule
    chronolog::IngestionQueue ingestionQueue;
    std::string keeper_csv_files_directory = KEEPER_CONF.EXTRACTOR_CONF.story_files_dir;
    // Instantiate KeeperGrapherDrainService
    tl::engine* extractionEngine = nullptr;
    try
    {
        int const drain_margo_progress_thread =
                env_positive_int("CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD", 1);
        int const drain_margo_rpc_threads = env_positive_int("CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS", 0);
        LOG_INFO("[ChronoKeeperInstance] Keeper-to-Grapher drain sender Margo progress_thread={} rpc_threads={}",
                 drain_margo_progress_thread,
                 drain_margo_rpc_threads);
        extractionEngine = new tl::engine(KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF,
                                          THALLIUM_CLIENT_MODE,
                                          drain_margo_progress_thread != 0,
                                          drain_margo_rpc_threads);

        std::stringstream s1;
        s1 << extractionEngine->self();
        LOG_INFO("[ChronoKeeperInstance] GroupID={} starting extraction engine at address {}",
                 keeper_group_id,
                 s1.str());
    }
    catch(tl::exception const&)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create extraction engine");
        return (-1);
    }

    chl::ServiceId grapherReceivingServiceId(KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF,
                                             KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP,
                                             KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT,
                                             KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID);

    chl::StoryChunkExtractorRDMA single_endpoint_rdma_extractor(*extractionEngine, grapherReceivingServiceId);
    chronolog::StoryChunkExtractionModule extractionModule(chl::LoggingExtractor(), single_endpoint_rdma_extractor);

    chronolog::KeeperDataStore theDataStore(ingestionQueue,
                                            extractionModule.getExtractionQueue(),
                                            KEEPER_CONF.DATA_STORE_CONF.max_story_chunk_size,
                                            KEEPER_CONF.DATA_STORE_CONF.story_chunk_duration_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.acceptance_window_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.inactive_story_delay_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.data_collection_poll_interval_us,
                                            [&single_endpoint_rdma_extractor](chl::StoryId const& story_id) {
                                                return single_endpoint_rdma_extractor.complete_story_drain(story_id);
                                            });

    // Instantiate KeeperRecordingService
    tl::engine* dataAdminEngine = nullptr;

    chronolog::DataStoreAdminService* keeperDataAdminService = nullptr;

    try
    {
        margo_instance_id collection_margo_id =
                margo_init(KEEPER_DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        std::stringstream s3;
        s3 << dataAdminEngine->self();
        LOG_DEBUG("[ChronoKeeperInstance] GroupID={} starting DataStoreAdminService at address {} with ProviderID={}",
                  keeper_group_id,
                  s3.str(),
                  datastore_service_provider_id);
        keeperDataAdminService =
                chronolog::DataStoreAdminService::CreateDataStoreAdminService(*dataAdminEngine,
                                                                              datastore_service_provider_id,
                                                                              theDataStore);
    }
    catch(tl::exception const&)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create DataStoreAdminService");
    }

    LOG_INFO("[ChronoKeeperInstance] DataStoreAdminService started successfully.");

    if(nullptr == keeperDataAdminService)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create DataStoreAdminService exiting");
        if(dataAdminEngine)
        {
            delete dataAdminEngine;
        }
        return (-1);
    }

    // Instantiate KeeperRecordingService
    tl::engine* recordingEngine = nullptr;
    chronolog::KeeperRecordingService* keeperRecordingService = nullptr;

    try
    {
        int const legacy_recording_margo_xstreams =
                env_positive_int("CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS", 1);
        int const recording_margo_progress_thread =
                env_nonnegative_int("CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD",
                                    legacy_recording_margo_xstreams > 0 ? 1 : 0);
        int const recording_margo_handlers = env_positive_int("CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS", 1);
        LOG_INFO("[ChronoKeeperInstance] KeeperRecordingService Margo progress_thread={} handlers={} legacy_xstreams={}",
                 recording_margo_progress_thread,
                 recording_margo_handlers,
                 legacy_recording_margo_xstreams);
        margo_instance_id margo_id = margo_init(KEEPER_RECORDING_SERVICE_NA_STRING.c_str(),
                                                MARGO_SERVER_MODE,
                                                recording_margo_progress_thread != 0,
                                                recording_margo_handlers);
        recordingEngine = new tl::engine(margo_id);

        std::stringstream s1;
        s1 << recordingEngine->self();
        LOG_INFO("[ChronoKeeperInstance] GroupID={} starting KeeperRecordingService at {} with provider_id {}",
                 keeper_group_id,
                 s1.str(),
                 recording_service_provider_id);
        keeperRecordingService =
                chronolog::KeeperRecordingService::CreateKeeperRecordingService(*recordingEngine,
                                                                                recording_service_provider_id,
                                                                                ingestionQueue,
                                                                                theDataStore);
    }
    catch(tl::exception const&)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService");
    }

    if(nullptr == keeperRecordingService)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService exiting");
        delete keeperDataAdminService;
        return (-1);
    }

    /// KeeperRegistryClient Set Up _____________________________________________________________________________________
    // create KeeperRegistryClient and register the new KeeperRecording service with the KeeperRegistry
    std::string KEEPER_REGISTRY_SERVICE_NA_STRING = KEEPER_CONF.VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF + "://" +
                                                    KEEPER_CONF.VISOR_REGISTRY_SERVICE_CONF.IP + ":" +
                                                    std::to_string(KEEPER_CONF.VISOR_REGISTRY_SERVICE_CONF.BASE_PORT);

    uint16_t KEEPER_REGISTRY_SERVICE_PROVIDER_ID = KEEPER_CONF.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::KeeperRegistryClient* keeperRegistryClient =
            chronolog::KeeperRegistryClient::CreateKeeperRegistryClient(*dataAdminEngine,
                                                                        KEEPER_REGISTRY_SERVICE_NA_STRING,
                                                                        KEEPER_REGISTRY_SERVICE_PROVIDER_ID);

    if(nullptr == keeperRegistryClient)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create KeeperRegistryClient; exiting");
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1);
    }

    /// Registration with ChronoVisor __________________________________________________________________________________
    // Keep retrying long enough to survive Visor-side delayed AdminClient cleanup after an unclean Keeper exit.
    int registration_status = chronolog::CL_ERR_UNKNOWN;
    int retries = env_positive_int("CHRONOLOG_KEEPER_REGISTRATION_RETRIES", 80);
    int const retry_sleep_ms = env_positive_int("CHRONOLOG_KEEPER_REGISTRATION_RETRY_SLEEP_MS", 100);
    int attempt = 0;
    while((chronolog::CL_SUCCESS != registration_status) && (attempt < retries))
    {
        registration_status = keeperRegistryClient->send_register_msg(
                chronolog::KeeperRegistrationMsg(keeperIdCard, dataStoreServiceId));
        attempt++;
        if(chronolog::CL_SUCCESS != registration_status && attempt < retries)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_sleep_ms));
        }
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to register with ChronoVisor after {} attempts over {} ms. Exiting.",
                     attempt,
                     attempt * retry_sleep_ms);
        delete keeperRegistryClient;
        delete keeperRecordingService;
        delete keeperDataAdminService;
        return (-1);
    }
    LOG_INFO("[ChronoKeeperInstance] Successfully registered with ChronoVisor.");
    chronolog::chrono_monitor::flush();

    /// Start data collection and extraction threads ___________________________________________________________________
    // services are successfully created and keeper process had registered with ChronoVisor
    // start all dataCollection and Extraction threads...
    tl::abt scope;
    int const keeper_data_collection_streams =
            env_positive_int("CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS", 3);
    int const keeper_data_collection_threads_per_stream =
            env_positive_int("CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM", 2);
    LOG_INFO("[ChronoKeeperInstance] Starting Keeper data collection streams={} threads_per_stream={}",
             keeper_data_collection_streams,
             keeper_data_collection_threads_per_stream);
    theDataStore.startDataCollection(keeper_data_collection_streams, keeper_data_collection_threads_per_stream);
    // start extraction streams & threads
    //storyExtractor.startExtractionThreads(2);
    int const keeper_extraction_threads = env_positive_int("CHRONOLOG_KEEPER_EXTRACTION_THREADS", 2);
    LOG_INFO("[ChronoKeeperInstance] Starting {} Keeper extraction thread(s)", keeper_extraction_threads);
    extractionModule.startExtraction(keeper_extraction_threads);


    /// Main loop for sending stats message until receiving SIGTERM ____________________________________________________
    // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    int const stats_period_ms = env_positive_int("CHRONOLOG_KEEPER_STATS_PERIOD_MS", 10000);
    int const stats_poll_ms = env_positive_int("CHRONOLOG_KEEPER_STATS_POLL_INTERVAL_MS", 100);
    std::string const append_stats_snapshot_request_path = KEEPER_CONF.LOG_CONF.LOGFILE + ".snapshot_request";
    std::thread append_stats_snapshot_thread([append_stats_snapshot_request_path]() {
        while(keep_running)
        {
            bool const file_snapshot_requested = ::access(append_stats_snapshot_request_path.c_str(), F_OK) == 0;
            if(file_snapshot_requested)
            {
                ::unlink(append_stats_snapshot_request_path.c_str());
            }
            if(append_stats_snapshot_requested || file_snapshot_requested)
            {
                append_stats_snapshot_requested = false;
                chronolog::KeeperAppendStats::instance().logSummary("signal_stats_snapshot");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if(append_stats_snapshot_requested)
        {
            append_stats_snapshot_requested = false;
            chronolog::KeeperAppendStats::instance().logSummary("signal_stats_snapshot");
        }
    });
    auto last_stats_sent = std::chrono::steady_clock::now() - std::chrono::milliseconds(stats_period_ms);
    while(keep_running)
    {
        auto const now = std::chrono::steady_clock::now();
        if(now - last_stats_sent >= std::chrono::milliseconds(stats_period_ms))
        {
            keeperRegistryClient->send_stats_msg(keeperStatsMsg);
            last_stats_sent = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(stats_poll_ms));
    }
    if(append_stats_snapshot_thread.joinable())
    {
        append_stats_snapshot_thread.join();
    }

    /// Unregister from ChronoVisor ____________________________________________________________________________________
    LOG_INFO("[ChronoKeeperInstance] Received shutdown signal. Initiating shutdown procedure.");
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
    extractionModule.shutdownExtraction();
    // these are not probably needed as thallium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();
    delete extractionEngine;
    delete recordingEngine;
    delete dataAdminEngine;
    LOG_INFO("[ChronoKeeperInstance] Shutdown completed. Exiting.");
    return exit_code;
}
