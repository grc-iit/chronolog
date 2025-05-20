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

namespace chl = chronolog;
namespace tl = thallium;

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

    // instantiate DataStoreAdminService

    // validate ip address, instantiate DataAdminService and create ServiceId to be included in RegistrationMsg

    chronolog::ServiceId playerAdminServiceId( confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF,
        confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.IP,
        confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT,
        confManager.PLAYER_CONF.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID);

    if( !playerAdminServiceId.is_valid())
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to start DataStoreAdminService. Invalid endpoint provided.");
        return (-1);
    }

    // Instantiate PlaybackService
    std::string PLAYBACK_SERVICE_PROTOCOL = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.PROTO_CONF;
    std::string PLAYBACK_SERVICE_IP = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.IP;
    uint16_t PLAYBACK_SERVICE_PORT = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.BASE_PORT;
    uint16_t playback_service_provider_id = confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID;

    // validate ip address, instantiate Playback Service and create IdCard

    chronolog::ServiceId playbackServiceId( confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.PROTO_CONF,
        confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.IP,
        confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.BASE_PORT,
        confManager.PLAYER_CONF.PLAYBACK_SERVICE_CONF.SERVICE_PROVIDER_ID);

    if( !playbackServiceId.is_valid())
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to start PlayerAdminService. Invalid endpoint provided.");
        return (-1);
    }


    tl::engine * playbackEngine = nullptr;
    chronolog::PlaybackService * playbackService = nullptr;
    chronolog::ArchiveReadingRequestQueue readingRequestQueue;

    try
    {
        std::string PLAYBACK_SERVICE_NA_STRING;

        playbackServiceId.get_service_as_string(PLAYBACK_SERVICE_NA_STRING);

        margo_instance_id playback_margo_id = margo_init(PLAYBACK_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1 , 1);

        playbackEngine = new tl::engine(playback_margo_id);

        LOG_DEBUG("[ChronoPlayer] starting PlaybackService at {}", chl::to_string(playbackServiceId));

        playbackService = chronolog::PlaybackService::CreatePlaybackService(*playbackEngine, playbackServiceId.getProviderId(),readingRequestQueue);
    }
    catch(tl::exception const & ex)
    {
        LOG_ERROR("[ChronoPlayer]  failed to create playbackService at {} exception:{}", chl::to_string(playbackServiceId), ex.what());
        playbackService = nullptr;
    }

    if(nullptr == playbackService)
    {
        LOG_CRITICAL("[ChronoPlayer] failed to create Playback Service at {}; exiting", chl::to_string(playbackServiceId));
        return (-1);
    }

    chronolog::RecordingGroupId recording_group_id = confManager.PLAYER_CONF.RECORDING_GROUP;

    // create PlayerIdCard to identify this Player process in ChronoVisor's Registry
    chronolog::PlayerIdCard playerIdCard(recording_group_id, playbackServiceId);
    LOG_INFO("[ChronoPlayer] created PlayerIdCard: {}", chl::to_string(playerIdCard));

    // Instantiate MemoryDataStore & ExtractorModule
    chronolog::StoryChunkIngestionQueue ingestionQueue;
    chronolog::StoryChunkExtractionQueue extractionQueue;

    chronolog::PlayerDataStore theDataStore(ingestionQueue, extractionQueue);

    tl::engine * dataAdminEngine = nullptr;

    chronolog::PlayerStoreAdminService * playerStoreAdminService = nullptr;

    try
    {
        std::string DATASTORE_SERVICE_NA_STRING;
        playerAdminServiceId.get_service_as_string(DATASTORE_SERVICE_NA_STRING);

        margo_instance_id collection_margo_id = margo_init(DATASTORE_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1
                                                           , 1);

        dataAdminEngine = new tl::engine(collection_margo_id);

        LOG_DEBUG("[ChronoPlayer] starting AdminService at {}", chl::to_string(playerAdminServiceId));
        
        playerStoreAdminService = chronolog::PlayerStoreAdminService::CreatePlayerStoreAdminService(*dataAdminEngine
                                                                                                , playerAdminServiceId.getProviderId()
                                                                                                , theDataStore);
    }
    catch(tl::exception const & ex)
    {
        LOG_ERROR("[ChronoPlayer]  failed to create DataStoreAdminService at {} exception:{}", chl::to_string(playerAdminServiceId), ex.what());
        playerStoreAdminService = nullptr;
    }

    if(nullptr == playerStoreAdminService)
    {
        LOG_CRITICAL("[ChronoPlayer] failed to create DataStoreAdminService at {} , exiting", chl::to_string(playerAdminServiceId));
        delete playbackService;
        return (-1);
    }


    LOG_INFO("[ChronoPlayer] started AdminService at {}", chl::to_string(playerAdminServiceId));

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

    chronolog::ArchiveReadingAgent * archiveReadingAgent = nullptr;

    std::string archive_path = confManager.GRAPHER_CONF.EXTRACTOR_CONF.story_files_dir;
    archiveReadingAgent = new chronolog::ArchiveReadingAgent(readingRequestQueue, archive_path);

    /// Registration with ChronoVisor __________________________________________________________________________________
    // try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = playerRegistryClient->send_register_msg(
            chronolog::PlayerRegistrationMsg(playerIdCard, playerAdminServiceId));
    //if the first attemp failes retry 
    int retries = 5;
    while((chronolog::CL_SUCCESS != registration_status) && (retries > 0))
    {
        registration_status = playerRegistryClient->send_register_msg(
                chronolog::PlayerRegistrationMsg(playerIdCard, playerAdminServiceId));
        sleep(5);
        retries--;
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoPlayer] Failed to register with ChronoVisor after multiple attempts. Exiting.");
        delete archiveReadingAgent;
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
    int NUMBER_ARCHIVE_READING_STREAMS = 1;
    archiveReadingAgent->startArchiveReading(NUMBER_ARCHIVE_READING_STREAMS); 

    /// Main loop for sending stats message until receiving SIGTERM ____________________________________________________
    // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::PlayerStatsMsg playerStatsMsg(playerIdCard);
    while(keep_running)
    {
        playerRegistryClient->send_stats_msg(playerStatsMsg);
        sleep(10);
    }

    /// Unregister from ChronoVisor ____________________________________________________________________________________
    // Unregister from the chronoVisor so that no new story requests would be coming
    playerRegistryClient->send_unregister_msg(playerIdCard);
    delete playerRegistryClient;

    /// Stop services and shut down ____________________________________________________________________________________
    LOG_INFO("[ChronoPlayer] Initiating shutdown procedures.");
    
    archiveReadingAgent->shutdownArchiveReading(); 
    delete archiveReadingAgent;
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
