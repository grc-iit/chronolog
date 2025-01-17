#include <unistd.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <signal.h>

#include "PlayerIdCard.h"
#include "PlayerRegClient.h"
#include "StoryChunkIngestionQueue.h"
#include "StoryChunkExtractionQueue.h"
#include "PlayerDataStore.h"
#include "PlayerStoreAdminService.h"
#include "ConfigurationManager.h"
#include "cmd_arg_parse.h"

#include "ArchiveReadingAgent.h"
#include "ArchiveReadingRequestQueue.h"
#include "PlaybackService.h"

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
        LOG_ERROR("[ChronoPlayer] Invalid IP address provided: {}", ip_string);
        return (-1);
    }

    // translate 32bit ip from network into the host byte order
    uint32_t ntoh_ip_addr = ntohl(sa.sin_addr.s_addr);
    uint16_t ntoh_port = port;
    endpoint = std::pair <uint32_t, uint16_t>(ntoh_ip_addr, ntoh_port);

    LOG_DEBUG("[ChronoPlayer] Service endpoint created: IP={}, Port={}", ip_string, port);
    return 1;
}

volatile sig_atomic_t keep_running = true;

void sigterm_handler(int)
{
    LOG_INFO("[ChronoPlayer] Received SIGTERM signal. Initiating shutdown procedure.");
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
    int result = chronolog::chrono_monitor::initialize(confManager.PLAYER_CONF.LOG_CONF.LOGTYPE
                                                       , confManager.PLAYER_CONF.LOG_CONF.LOGFILE
                                                       , confManager.PLAYER_CONF.LOG_CONF.LOGLEVEL
                                                       , confManager.PLAYER_CONF.LOG_CONF.LOGNAME
                                                       , confManager.PLAYER_CONF.LOG_CONF.LOGFILESIZE
                                                       , confManager.PLAYER_CONF.LOG_CONF.LOGFILENUM
                                                       , confManager.PLAYER_CONF.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }
    LOG_INFO("Running ChronoPlayer ");

    // Instantiate MemoryDataStore
    // instantiate DataStoreAdminService

    /// DataStoreAdminService setup ____________________________________________________________________________________
    std::string datastore_service_ip = confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP;
    int datastore_service_port = confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT;
    std::string DATASTORE_SERVICE_NA_STRING =
            confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF + "://" + datastore_service_ip + ":" +
            std::to_string(datastore_service_port);

    uint16_t datastore_service_provider_id = confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::service_endpoint datastore_endpoint;
    // validate ip address, instantiate DataAdminService and create ServiceId to be included in RegistrationMsg

    if(-1 == service_endpoint_from_dotted_string(datastore_service_ip, datastore_service_port, datastore_endpoint))
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to start DataStoreAdminService. Invalid endpoint provided.");
        return (-1);
    }
    LOG_INFO("[ChronoPlayer] DataStoreAdminService started successfully.");

    // Instantiate PlaybackService
    std::string PLAYBACK_SERVICE_PROTOCOL = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.PROTO_CONF;
    std::string PLAYBACK_SERVICE_IP = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.IP;
    uint16_t PLAYBACK_SERVICE_PORT = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.BASE_PORT;
    uint16_t playback_service_provider_id = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID;

    // validate ip address, instantiate Playback Service and create IdCard

    chronolog::service_endpoint playback_endpoint;
    if(-1 == service_endpoint_from_dotted_string(PLAYBACK_SERVICE_IP, PLAYBACK_SERVICE_PORT, playback_endpoint))
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to start PlaybackService. Invalid endpoint provided.");
        return (-1);
    }

    tl::engine * playbackEngine = nullptr;
    chronolog::PlaybackService * playbackService = nullptr;
    chronolog::ArchiveReadingRequestQueue readingRequestQueue;

    try
    {
        std::string PLAYBACK_SERVICE_NA_STRING = std::string(confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.PROTO_CONF) 
            + "://" + std::string(confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.IP) + ":" +
            std::to_string(confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.BASE_PORT);

        margo_instance_id playback_margo_id = margo_init(PLAYBACK_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1
                                                           , 1);

        playbackEngine = new tl::engine(playback_margo_id);

        std::stringstream s3;
        s3 << playbackEngine->self();
        LOG_DEBUG("[ChronoPlayer] starting PlaybackService at address {} with ProviderID={}", s3.str()
                  , playback_service_provider_id);

        playbackService = chronolog::PlaybackService::CreatePlaybackService(*playbackEngine, playback_service_provider_id,readingRequestQueue);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoPlayer]  failed to create playbackService");
        playbackService = nullptr;
    }

    if(nullptr == playbackService)
    {
        LOG_CRITICAL("[ChronoPlayer] failed to create Playback Service exiting");
        return (-1);
    }

    chronolog::RecordingGroupId recording_group_id = confManager.PLAYER_CONF.RECORDING_GROUP;
 /*   std::string RECORDING_SERVICE_PROTOCOL = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.PROTO_CONF;
    std::string RECORDING_SERVICE_IP = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.IP;
    uint16_t RECORDING_SERVICE_PORT = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = confManager.GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.SERVICE_PROVIDER_ID;

    std::string RECORDING_SERVICE_NA_STRING =
            std::string(RECORDING_SERVICE_PROTOCOL) + "://" + std::string(RECORDING_SERVICE_IP) + ":" +
            std::to_string(RECORDING_SERVICE_PORT);

    // validate ip address, instantiate Recording Service and create IdCard

    chronolog::service_endpoint recording_endpoint;
    if(-1 == service_endpoint_from_dotted_string(RECORDING_SERVICE_IP, RECORDING_SERVICE_PORT, recording_endpoint))
    {
        LOG_CRITICAL("[ChronoGrapher] Failed to start RecordingService. Invalid endpoint provided.");
        return (-1);
    }
    LOG_INFO("[ChronoGrapher] RecordingService started successfully.");
*/
    // create PlayerIdCard to identify this Player process in ChronoVisor's Registry
    chronolog::PlayerIdCard processIdCard(recording_group_id, datastore_endpoint.first, datastore_endpoint.second
                                           , datastore_service_provider_id);

    std::stringstream process_id_string;
    process_id_string << processIdCard;
    LOG_INFO("[ChronoPlayer] PlayerIdCard: {}", process_id_string.str());

    // Instantiate MemoryDataStore & ExtractorModule
    chronolog::StoryChunkIngestionQueue ingestionQueue;
    chronolog::StoryChunkExtractionQueue extractionQueue;

    chronolog::PlayerDataStore theDataStore(ingestionQueue, extractionQueue);

    chronolog::ServiceId collectionServiceId(datastore_endpoint.first, datastore_endpoint.second
                                             , datastore_service_provider_id);
    tl::engine * dataAdminEngine = nullptr;

    chronolog::PlayerStoreAdminService * playerStoreAdminService = nullptr;

    try
    {
        margo_instance_id collection_margo_id = margo_init(DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1
                                                           , 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        std::stringstream s3;
        s3 << dataAdminEngine->self();
        LOG_DEBUG("[ChronoPlayer] starting DataStoreAdminService at address {} with ProviderID={}", s3.str()
                  , datastore_service_provider_id);
        playerStoreAdminService = chronolog::PlayerStoreAdminService::CreatePlayerStoreAdminService(*dataAdminEngine
                                                                                                , datastore_service_provider_id
                                                                                                , theDataStore);
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[ChronoPlayer]  failed to create DataStoreAdminService");
        playerStoreAdminService = nullptr;
    }

    if(nullptr == playerStoreAdminService)
    {
        LOG_CRITICAL("[ChronoPlayer] failed to create DataStoreAdminService exiting");
        delete playbackService;
        return (-1);
    }


    /// RegistryClient SetUp _____________________________________________________________________________________
    // create RegistryClient and register the new Recording service with the Registry
    std::string REGISTRY_SERVICE_NA_STRING = confManager.PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.PROTO_CONF + "://" +
                                             confManager.PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.IP + ":" +
                                             std::to_string(
                                                     confManager.PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.BASE_PORT);

    uint16_t REGISTRY_SERVICE_PROVIDER_ID = confManager.PLAYER_CONF.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID;

    chronolog::PlayerRegistryClient * playerRegistryClient = chronolog::PlayerRegistryClient::CreateRegistryClient(
            *dataAdminEngine, REGISTRY_SERVICE_NA_STRING, REGISTRY_SERVICE_PROVIDER_ID);

    if(nullptr == playerRegistryClient)
    {
        LOG_CRITICAL("[ChronoPlayer] failed to create RegistryClient; exiting");
        delete playerStoreAdminService;
        delete playbackService;
        return (-1);
    }

    /// Registration with ChronoVisor __________________________________________________________________________________
    // try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = playerRegistryClient->send_register_msg(
            chronolog::PlayerRegistrationMsg(processIdCard, collectionServiceId));
    //if the first attemp failes retry 
    int retries = 5;
    while((chronolog::CL_SUCCESS != registration_status) && (retries > 0))
    {
        registration_status = playerRegistryClient->send_register_msg(
                chronolog::PlayerRegistrationMsg(processIdCard, collectionServiceId));
        sleep(5);
        retries--;
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to register with ChronoVisor after multiple attempts. Exiting.");
        delete playerRegistryClient;
        delete playerStoreAdminService;
        delete playbackService;
        return (-1);
    }
    LOG_INFO("[ChronoPlayer] Successfully registered with ChronoVisor.");

    /// Start data collection and extraction threads ___________________________________________________________________
    // services are successfully created and keeper process had registered with ChronoVisor
    // start all dataCollection and Extraction threads...
    tl::abt scope;
   // theDataStore.startDataCollection(3);
    // start extraction streams & threads
    //storyExtractor.startExtractionThreads(2);

    /// Main loop for sending stats message until receiving SIGTERM ____________________________________________________
    // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::PlayerStatsMsg playerStatsMsg(processIdCard);
    while(keep_running)
    {
        playerRegistryClient->send_stats_msg(playerStatsMsg);
        sleep(10);
    }

    /// Unregister from ChronoVisor ____________________________________________________________________________________
    // Unregister from the chronoVisor so that no new story requests would be coming
    playerRegistryClient->send_unregister_msg(processIdCard);
    delete playerRegistryClient;

    /// Stop services and shut down ____________________________________________________________________________________
    LOG_INFO("[ChronoPlayer] Initiating shutdown procedures.");
    // Stop recording events
    delete playerStoreAdminService;
    delete playbackService;
    // Shutdown the Data Collection
    //theDataStore.shutdownDataCollection();
    // Shutdown extraction module
    // drain extractionQueue and stop extraction xStreams
    //storyExtractor.shutdownExtractionThreads();
    // these are not probably needed as thallium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();
   // delete recordingEngine;
    delete dataAdminEngine;
    delete playbackEngine;
    LOG_INFO("[ChronoPlayer] Shutdown completed. Exiting.");
    return exit_code;
}
