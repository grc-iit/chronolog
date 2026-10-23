#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/uio.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <cstdint>

#include <KeeperRecordingService.h>
#include <KeeperRegClient.h>
#include <IngestionQueue.h>
#include <StoryChunkExtractionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
#include <KeeperDurableWriter.h>
#include <KeeperWal.h>
#include <WalFileSink.h>
#include <WalMountCheck.h>
#include <WalRecovery.h>
#include <DataStoreAdminService.h>
#include <cmd_arg_parse.h>
#include <StoryChunkExtractionModule.h>
#include <KeeperExtractionChain.h>
#include <KeeperExtractionEngine.h>
#include <ConfigurationManager.h>
#include <ChronoKeeperConfiguration.h>

namespace chl = chronolog;
namespace tl = thallium;

// The file a keeper with the WAL on leaves in its WAL directory when it exits:
// "pending_events=<n> pending_bytes=<b>", the durable events the grapher had
// not confirmed. The deploy scripts read it after a stop; a missing file means
// the keeper crashed or was killed. Removed at startup.
static std::string const WAL_EXIT_SUMMARY = "exit-summary";

// Logs where the WAL directory lives and every warning about it: a shared or
// network file system, tmpfs, overlayfs, the root device, free space below the
// reserve. The keeper starts either way.
static void check_wal_mount(chl::WalConf const& wal_conf)
{
    chl::WalMountInfo info;
    int err = chl::inspectWalDirectory(wal_conf.dir, info);
    if(err != 0)
    {
        LOG_WARNING("[ChronoKeeperInstance] Cannot inspect the file system of WAL directory {}: {}",
                    wal_conf.dir,
                    std::strerror(err));
        return;
    }
    uint64_t const reserve_bytes =
            chl::walReserveBytes(info.total_bytes, wal_conf.reserve_min_bytes, wal_conf.reserve_percent);
    LOG_INFO("[ChronoKeeperInstance] WAL directory {} is on {}: {} bytes free of {}, reserve {} bytes",
             wal_conf.dir,
             info.fs_name,
             info.free_bytes,
             info.total_bytes,
             reserve_bytes);
    for(std::string const& warning: chl::walMountWarnings(info, reserve_bytes))
    {
        LOG_WARNING("[ChronoKeeperInstance] WAL directory {}: {}", wal_conf.dir, warning);
    }
}

static void log_wal_replay(std::string const& dir,
                           chl::RecoveredState const& recovered,
                           std::size_t chunks,
                           std::chrono::steady_clock::duration elapsed)
{
    std::size_t events = 0;
    for(auto const& [wal_story_id, story]: recovered.stories)
    {
        events += story.events.size();
        if(story.events.empty())
        {
            continue;
        }
        // events are ordered by time first
        LOG_INFO("[ChronoKeeperInstance] WAL replay: StoryID={} {}/{} (WAL StoryID={}): {} events from {} to {}",
                 chl::recoveredStoryId(story),
                 story.has_names ? story.chronicle : std::string("<no names>"),
                 story.has_names ? story.story : std::string("<no names>"),
                 wal_story_id,
                 story.events.size(),
                 story.events.begin()->second.time(),
                 story.events.rbegin()->second.time());
    }
    LOG_INFO("[ChronoKeeperInstance] WAL replay from {}: {} segments, {} bytes read, oldest segment {} s old, "
             "{} stories, {} events in {} chunks, took {} ms",
             dir,
             recovered.segments.size(),
             recovered.bytes_read,
             recovered.oldest_segment_age_secs,
             recovered.stories.size(),
             events,
             chunks,
             std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

// Opens the WAL directory, checks its mount, reads it back, starts the WAL on
// what it holds, makes it the retention store's WalObserver and hands the
// recovered events to the retention store as sealed chunks. nullptr, with the
// reason logged, when the keeper must not start.
static std::unique_ptr<chl::KeeperWal> start_keeper_wal(chl::WalConf const& wal_conf,
                                                        std::string const& keeper_endpoint,
                                                        chl::KeeperChunkRetentionStore& tail_store,
                                                        chl::KeeperDataStore& data_store)
{
    auto const started_at = std::chrono::steady_clock::now();
    auto wal = std::make_unique<chl::KeeperWal>(wal_conf, keeper_endpoint, chl::makePosixWalFileSink());

    int err = wal->fileSink().openDirectory(wal_conf.dir);
    if(err != 0)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Cannot open WAL directory {}: {}", wal_conf.dir, std::strerror(err));
        return nullptr;
    }
    check_wal_mount(wal_conf);

    chl::RecoveredState recovered;
    std::string error;
    int const recovery_status = chl::recoverWal(wal->fileSink(), wal->segmentManager(), recovered, error);
    if(wal->segmentManager().isOpen())
    {
        // This keeper holds the lock now, so a summary left by the last one is
        // stale. It goes even when recovery refuses, so a stop after a failed
        // start does not read the last clean exit's counts.
        err = wal->fileSink().remove(WAL_EXIT_SUMMARY);
        if(err == 0)
        {
            err = wal->fileSink().syncDirectory();
        }
        if(err != 0 && err != ENOENT)
        {
            LOG_WARNING("[ChronoKeeperInstance] Cannot remove {} from WAL directory {}: {}",
                        WAL_EXIT_SUMMARY,
                        wal_conf.dir,
                        std::strerror(err));
        }
    }
    if(recovery_status != chl::CL_SUCCESS)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] WAL recovery failed: {}. Exiting.", error);
        return nullptr;
    }

    // The WAL starts and becomes the observer before the replay, so an event
    // the replay cannot place is released like any other discarded event.
    if(wal->start(recovered, chl::recoveredStoryId) != chl::CL_SUCCESS)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Cannot start the WAL in {}. Exiting.", wal_conf.dir);
        return nullptr;
    }
    tail_store.setWalObserver(wal.get());
    std::size_t const chunks = data_store.replayRecoveredEvents(recovered);
    log_wal_replay(wal_conf.dir, recovered, chunks, std::chrono::steady_clock::now() - started_at);
    return wal;
}

// Writes the exit summary as <name>.partial, data-syncs it, renames it into
// place and syncs the directory, so a reader finds the whole line or no file.
// 0, or the errno of the step `what` names.
static int write_wal_exit_summary(chl::WalFileSink& sink, chl::WalStats const& stats, std::string& what)
{
    std::string const line = "pending_events=" + std::to_string(stats.pending_events) +
                             " pending_bytes=" + std::to_string(stats.pending_bytes) + "\n";
    // WalSegmentManager::open removes a leftover *.partial at the next start
    std::string const partial = WAL_EXIT_SUMMARY + ".partial";
    what = "create " + partial;
    int err = sink.createZeroed(partial, 0);
    int fd = -1;
    if(err == 0)
    {
        what = "open " + partial;
        err = sink.openForWrite(partial, fd);
    }
    if(err == 0)
    {
        iovec iov{const_cast<char*>(line.data()), line.size()};
        what = "write " + partial;
        err = sink.writeAt(fd, 0, &iov, 1);
        if(err == 0)
        {
            what = "fdatasync " + partial;
            err = sink.syncData(fd);
        }
        sink.closeFile(fd);
    }
    if(err == 0)
    {
        what = "rename " + partial;
        err = sink.rename(partial, WAL_EXIT_SUMMARY);
    }
    if(err == 0)
    {
        what = "fsync the directory";
        err = sink.syncDirectory();
    }
    return err;
}

// Called once the recording service takes no more requests: commits and
// answers what is queued, stops the WAL, and leaves the exit summary.
static void stop_keeper_wal(chl::KeeperWal& wal)
{
    wal.stop();
    chl::WalStats const stats = wal.stats();
    std::string what;
    int err = write_wal_exit_summary(wal.fileSink(), stats, what);
    if(err != 0)
    {
        LOG_ERROR("[ChronoKeeperInstance] Cannot write {} in WAL directory {}: {} failed: {}",
                  WAL_EXIT_SUMMARY,
                  wal.directory(),
                  what,
                  std::strerror(err));
    }
    if(stats.pending_events > 0)
    {
        LOG_WARNING("[ChronoKeeperInstance] Exiting with {} unconfirmed durable events ({} bytes) in WAL directory {}; "
                    "a keeper started on that directory replays them",
                    stats.pending_events,
                    stats.pending_bytes,
                    wal.directory());
    }
    else
    {
        LOG_INFO("[ChronoKeeperInstance] Exiting with {} unconfirmed durable events ({} bytes) in WAL directory {}",
                 stats.pending_events,
                 stats.pending_bytes,
                 wal.directory());
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

void sigterm_handler(int)
{
    LOG_INFO("[ChronoKeeperInstance] Received SIGTERM signal. Initiating shutdown procedure.");
    keep_running = false;
    return;
}

///////////////////////////////////////////////

int main(int argc, char** argv)
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
    std::string KEEPER_RECORDING_SERVICE_PROTOCOL = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF;
    std::string KEEPER_RECORDING_SERVICE_IP = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP;
    uint16_t KEEPER_RECORDING_SERVICE_PORT = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT;
    uint16_t recording_service_provider_id = KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID;


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
            chronolog::ServiceId(KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT,
                                 KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID));

    std::string KEEPER_RECORDING_SERVICE_NA_STRING;
    keeperIdCard.getRecordingServiceId().get_service_as_string(KEEPER_RECORDING_SERVICE_NA_STRING);

    LOG_INFO("[ChronoKeeperInstance] KeeperIdCard: {}", chronolog::to_string(keeperIdCard));


    // Instantiate ChronoKeeper  ExtractionModule

    tl::engine* extractionEngine = nullptr;
    // default extraction engine fabric is "ofi+sockets"
    std::string extraction_engine_protocol = KEEPER_CONF.EXTRACTION_MODULE_CONF.extraction_protocol;


    // if single_endpoint_rdma_extractor or dual_endpoint_rdma_extractor
    // are configured then extraction engine needs to be instantiated with
    // the protocol of the grapher receiving endpoint

    try
    {
        extractionEngine = chl::createExtractionEngine(extraction_engine_protocol);

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

    std::string log_string;

    LOG_INFO("[ChronoKeeperInstance] Initializing StoryChunkExtractionModule with {}",
             KEEPER_CONF.EXTRACTION_MODULE_CONF.to_string(log_string));

    chl::StoryChunkExtractionModule<chl::ChronoKeeperExtractionChain> theExtractionModule(
            KEEPER_CONF.EXTRACTION_MODULE_CONF.extraction_stream_count);

    theExtractionModule.getExtractionChain().activate(keeperIdCard.getRecordingServiceId(),
                                                      extractionEngine,
                                                      KEEPER_CONF.EXTRACTION_MODULE_CONF);

    // Every grapher-bound chunk carries this keeper's DataStoreAdminService
    // identity so the grapher knows where to push watermark reports.
    theExtractionModule.getExtractionChain().set_watermark_reporter(dataStoreServiceId);

    theExtractionModule.initialize(KEEPER_CONF.EXTRACTION_MODULE_CONF.extraction_stream_count);

    if(!theExtractionModule.is_initialized())
    {
        LOG_ERROR("[ChronoKeeperInstance] StoryChunkExtractionModule failed to initialize, exiting");
        return (-1);
    }

    // Instantiate the chunk retention store (owns every sealed chunk) and DataStore.
    // tail_capacity = max number of most-recent sealed events indexed per story
    // for last-N playback (DataStoreInternals.tail_capacity, default 65536).
    // retention_cap_mb = soft cap on retained-chunk memory; on exceed the store
    // WARNs (watermark lagging) but never drops unpersisted data
    // (DataStoreInternals.retention_cap_mb, default 4096, 0 disables the warning).
    // live_tail_read (default false): when true, tail reads also serve events from
    // the active/unsealed timeline, cutting write-to-visible latency below the
    // seal window at the cost of provisional (eventually-consistent) reads.
    const std::size_t keeper_tail_capacity = static_cast<std::size_t>(KEEPER_CONF.DATA_STORE_CONF.tail_capacity);
    const std::size_t keeper_retention_cap_mb = static_cast<std::size_t>(KEEPER_CONF.DATA_STORE_CONF.retention_cap_mb);
    // archive_visibility_delay_secs (default 10): how long after the grapher
    // reports a chunk written the store keeps it and serves it as unconfirmed,
    // so a player that has not listed the new archive file yet still replays it.
    const int archive_visibility_delay_secs = KEEPER_CONF.DATA_STORE_CONF.archive_visibility_delay_secs;
    // The keeper's WAL for durable writes, when WAL.enabled. Declared before
    // the retention store, which keeps a pointer to it as its WalObserver, so
    // it outlives the store on every return from main.
    std::unique_ptr<chl::KeeperWal> theWal;
    chronolog::KeeperChunkRetentionStore theTailStore(theExtractionModule.getExtractionQueue(),
                                                      keeper_tail_capacity,
                                                      keeper_retention_cap_mb,
                                                      KEEPER_CONF.DATA_STORE_CONF.live_tail_read,
                                                      std::chrono::seconds(archive_visibility_delay_secs));

    // The extraction chain reports every drain outcome (grapher ack / transfer
    // failure) back to the store; chunks are never freed by the drain loop.
    theExtractionModule.getExtractionChain().attachRetentionStore(&theTailStore);

    // Instantiate KeeperDataStore
    chronolog::IngestionQueue ingestionQueue;
    chronolog::KeeperDataStore theDataStore(ingestionQueue,
                                            theExtractionModule.getExtractionQueue(),
                                            theTailStore,
                                            KEEPER_CONF.DATA_STORE_CONF.max_story_chunk_size,
                                            KEEPER_CONF.DATA_STORE_CONF.story_chunk_duration_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.acceptance_window_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.inactive_story_delay_secs,
                                            KEEPER_CONF.DATA_STORE_CONF.watermark_resend_timeout_secs);

    // Recover the WAL before any service takes a request or the keeper
    // registers: the recovered events become sealed chunks the extraction
    // module ships once it starts, and their WAL records stay pending until
    // the grapher confirms them.
    if(KEEPER_CONF.WAL_CONF.enabled)
    {
        theWal = start_keeper_wal(KEEPER_CONF.WAL_CONF, KEEPER_RECORDING_SERVICE_NA_STRING, theTailStore, theDataStore);
        if(theWal == nullptr)
        {
            return (-1);
        }
    }
    // Every return from here stops the WAL and leaves its exit summary.
    auto stop_wal = [&theWal]()
    {
        if(theWal != nullptr)
        {
            stop_keeper_wal(*theWal);
        }
    };

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
        stop_wal();
        return (-1);
    }

    // The keeper side of record_event_durable. With no WAL it answers every
    // durable write CL_ERR_DURABILITY_UNAVAILABLE and ingests nothing.
    chronolog::KeeperDurableWriter theDurableWriter(theWal.get(),
                                                    ingestionQueue,
                                                    theTailStore,
                                                    theDataStore,
                                                    KEEPER_CONF.WAL_CONF.durableCapBytes());

    // Instantiate KeeperRecordingService
    tl::engine* recordingEngine = nullptr;
    chronolog::KeeperRecordingService* keeperRecordingService = nullptr;

    try
    {
        int rpc_thread_count = static_cast<int>(KEEPER_CONF.KEEPER_RECORDING_SERVICE_CONF.INGESTION_THREAD_COUNT);
        margo_instance_id margo_id =
                margo_init(KEEPER_RECORDING_SERVICE_NA_STRING.c_str(), MARGO_SERVER_MODE, 1, rpc_thread_count);
        recordingEngine = new tl::engine(margo_id);

        std::stringstream s1;
        s1 << recordingEngine->self();
        LOG_INFO("[ChronoKeeperInstance] GroupID={} starting KeeperRecordingService at {} with provider_id {}",
                 keeper_group_id,
                 s1.str(),
                 recording_service_provider_id);
        keeperRecordingService = chronolog::KeeperRecordingService::CreateKeeperRecordingService(
                *recordingEngine,
                recording_service_provider_id,
                ingestionQueue,
                theTailStore,
                [&theDurableWriter](chl::ChronicleName const& chronicle,
                                    chl::StoryName const& story,
                                    chl::LogEvent const& event)
                { return theDurableWriter.write(chronicle, story, event); });
    }
    catch(tl::exception const&)
    {
        LOG_ERROR("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService");
    }

    if(nullptr == keeperRecordingService)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Keeper failed to create KeeperRecordingService exiting");
        delete keeperDataAdminService;
        stop_wal();
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
        stop_wal();
        return (-1);
    }

    /// Registration with ChronoVisor __________________________________________________________________________________
    // try to register with chronoVisor a few times than log ERROR and exit...
    int registration_status = chronolog::CL_ERR_UNKNOWN;
    constexpr int max_registration_attempts = 5;
    int retries = max_registration_attempts;
    int attempt = 0;
    while((chronolog::CL_SUCCESS != registration_status) && (retries > 0))
    {
        registration_status = keeperRegistryClient->send_register_msg(
                chronolog::KeeperRegistrationMsg(keeperIdCard, dataStoreServiceId));
        retries--;
        attempt++;
        if(chronolog::CL_SUCCESS != registration_status)
        {
            LOG_WARNING("[ChronoKeeperInstance] Registration attempt {} of {} failed with status {}",
                        attempt,
                        max_registration_attempts,
                        registration_status);
            if(retries > 0)
            {
                sleep(5);
            }
        }
    }

    if(chronolog::CL_SUCCESS != registration_status)
    {
        LOG_CRITICAL("[ChronoKeeperInstance] Failed to register with ChronoVisor after multiple attempts. Exiting.");
        delete keeperRegistryClient;
        delete keeperRecordingService;
        delete keeperDataAdminService;
        stop_wal();
        return (-1);
    }
    LOG_INFO("[ChronoKeeperInstance] Successfully registered with ChronoVisor.");

    /// Start data collection and extraction threads ___________________________________________________________________
    // services are successfully created and keeper process had registered with ChronoVisor
    // start all dataCollection and Extraction threads...
    tl::abt scope;
    theDataStore.startDataCollection(3);
    // start extraction streams & threads
    theExtractionModule.startExtraction();


    /// Main loop for sending stats message until receiving SIGTERM ____________________________________________________
    // now we are ready to ingest records coming from the storyteller clients ....
    // main thread would be sending stats message until keeper process receives
    // sigterm signal
    chronolog::KeeperStatsMsg keeperStatsMsg(keeperIdCard);
    while(keep_running)
    {
        keeperRegistryClient->send_stats_msg(keeperStatsMsg);
        if(theWal != nullptr)
        {
            chl::WalStats const wal_stats = theWal->stats();
            LOG_INFO("[KeeperWal] stats groups={} events={} bytes={} max_events_per_group={} pending_events={} "
                     "pending_bytes={} segments={}",
                     wal_stats.groups,
                     wal_stats.events,
                     wal_stats.bytes,
                     wal_stats.max_events_per_group,
                     wal_stats.pending_events,
                     wal_stats.pending_bytes,
                     wal_stats.segments);
        }
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
    // Shutdown the Data Collection
    theDataStore.shutdownDataCollection();
    // Data collection has finished, so nothing new will seal. Before extraction
    // stops, wait for the grapher to confirm every chunk this keeper holds
    // written: an acked chunk may still exist only in the grapher's memory, and a
    // send can fail after its chunk was handed over. The wait sends unacked
    // chunks again, so the extraction module must still be draining, and it
    // learns of confirmations through watermark reports, so the data admin
    // service stays up until it ends. ~KeeperChunkRetentionStore logs and frees
    // whatever is still unconfirmed.
    // TODO(shutdown recovery): with the template values (shutdown 150 s, resend
    // 300 s) a chunk acked shortly before SIGTERM stays younger than the stall
    // age for the whole wait, so requeueStalled never fires here. Settling
    // deliveries already in flight works -- that is the common case -- but a
    // delivery whose grapher-side write failed cannot be rescued: its receipt
    // never settles, the wait times out, and the destructor frees the last copy
    // with a warning. Recovering it needs the wait to outlast a stall age plus a
    // write window (resend_age + write_window <= shutdown_confirm, so >= 390 s
    // with today's numbers), or a shorter stall age used only during shutdown --
    // which the keeper cannot derive, since the grapher's window is not in its
    // configuration. Documented rather than changed; see the shutdown-wait
    // recovery follow-up.
    const int shutdown_confirm_timeout_secs = KEEPER_CONF.DATA_STORE_CONF.shutdown_confirm_timeout_secs;
    if(!theTailStore.waitUntilDurable(std::chrono::seconds(shutdown_confirm_timeout_secs),
                                      std::chrono::seconds(1),
                                      std::chrono::seconds(KEEPER_CONF.DATA_STORE_CONF.watermark_resend_timeout_secs)))
    {
        LOG_WARNING("[ChronoKeeperInstance] The grapher did not confirm every chunk written within "
                    "shutdown_confirm_timeout_secs={}",
                    shutdown_confirm_timeout_secs);
    }
    delete keeperDataAdminService;
    // No request and no confirmation reaches the WAL any more: commit and
    // answer what is queued, and leave the exit summary with the events the
    // grapher did not confirm. A keeper started on the directory replays them.
    stop_wal();
    // Shutdown extraction module
    // drain extractionQueue and stop extraction xStreams
    theExtractionModule.shutdownExtraction();
    // Nothing calls into the retention store now, so its observer can go, and
    // the WAL with it while Argobots is still up.
    theTailStore.setWalObserver(nullptr);
    theWal.reset();

    // these are not probably needed as thallium handles the engine finalization...
    //  recordingEngine.finalize();
    //  collectionEngine.finalize();
    delete extractionEngine;
    delete recordingEngine;
    delete dataAdminEngine;
    LOG_INFO("[ChronoKeeperInstance] Shutdown completed. Exiting.");
    return exit_code;
}
