#include <signal.h>
#include <sys/uio.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <KeeperRecordingService.h>
#include <KeeperDurableWriter.h>
#include <KeeperWal.h>
#include <WalFileSink.h>
#include <WalMountCheck.h>
#include <WalRecovery.h>
#include <KeeperRegClient.h>
#include <KeeperDataStore.h>
#include <DataStoreAdminService.h>
#include <KeeperExtractionChain.h>
#include <StoryChunkExtractionModule.h>
#include <ConfigurationManager.h>
#include <ChronoKeeperConfiguration.h>
#include <cmd_arg_parse.h>
#include <private/ServiceLifecycle.h>
#include <private/LifecyclePublisher.h>
#include <private/ManagedStopBarrier.h>
namespace chl = chronolog;
namespace rpc = chronolog::rpc;
using namespace std::chrono_literals;

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
        // events are keyed by (time, client id, index); the replay has moved
        // their records out, so read the times from the keys
        LOG_INFO("[ChronoKeeperInstance] WAL replay: StoryID={} {}/{} (WAL StoryID={}): {} events from {} to {}",
                 chl::recoveredStoryId(story),
                 story.has_names ? story.chronicle : std::string("<no names>"),
                 story.has_names ? story.story : std::string("<no names>"),
                 wal_story_id,
                 story.events.size(),
                 std::get<0>(story.events.begin()->first),
                 std::get<0>(story.events.rbegin()->first));
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

namespace
{
volatile sig_atomic_t stop_requests = 0;
void request_stop(int)
{
    if(stop_requests < SIG_ATOMIC_MAX)
        ++stop_requests;
}
struct KeeperData
{
    chl::KeeperConfiguration conf;
    chl::KeeperIdCard id;
    chl::ServiceId admin;
    // The WAL for durable writes, when WAL.enabled. Declared before the
    // retention store, which keeps a pointer to it as its WalObserver.
    std::unique_ptr<chl::KeeperWal> wal;
    chl::StoryChunkExtractionModule<chl::ChronoKeeperExtractionChain> extraction;
    chl::KeeperChunkRetentionStore retained;
    chl::IngestionQueue ingestion;
    chl::KeeperDataStore store;
    std::unique_ptr<chl::KeeperDurableWriter> durableWriter;
    std::unique_ptr<chl::KeeperRegistryClient> registry;
    bool started = false, extractionReady = false, unregistered = false;
    std::array<std::shared_ptr<rpc::LifecycleStep>, 3> initialized;
    std::shared_ptr<rpc::LifecycleStep> walStart, start, stopCollection, durable, stopExtraction, walStop,
            publisherPermit;
    explicit KeeperData(chl::KeeperConfiguration const& c)
        : conf(c)
        , id(c.RECORDING_GROUP,
             chl::ServiceId(c.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.PROTO_CONF,
                            c.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.IP,
                            c.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.BASE_PORT,
                            c.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID))
        , admin(c.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF,
                c.DATA_STORE_ADMIN_SERVICE_CONF.IP,
                c.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT,
                c.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID)
        , extraction(c.EXTRACTION_MODULE_CONF.extraction_stream_count)
        , retained(extraction.getExtractionQueue(),
                   c.DATA_STORE_CONF.tail_capacity,
                   c.DATA_STORE_CONF.retention_cap_mb,
                   c.DATA_STORE_CONF.live_tail_read,
                   std::chrono::seconds(c.DATA_STORE_CONF.archive_visibility_delay_secs))
        , store(ingestion,
                extraction.getExtractionQueue(),
                retained,
                c.DATA_STORE_CONF.max_story_chunk_size,
                c.DATA_STORE_CONF.story_chunk_duration_secs,
                c.DATA_STORE_CONF.acceptance_window_secs,
                c.DATA_STORE_CONF.inactive_story_delay_secs,
                c.DATA_STORE_CONF.watermark_resend_timeout_secs)
    {}
};
std::string address(chl::RPCProviderConf const& conf)
{
    return conf.PROTO_CONF + "://" + conf.IP + ":" + std::to_string(conf.BASE_PORT);
}
} // namespace
int main(int argc, char** argv)
{
    auto startupEntered = rpc::RpcDeadline::Clock::now();
    signal(SIGTERM, request_stop);
    std::string config = parse_conf_path_arg(argc, argv);
    if(config.empty())
        return 1;
    chl::ConfigurationManager manager(config);
    chl::KeeperConfiguration conf;
    if(conf.parseJsonConf(manager.KEEPER_JSON_CONF) != chl::CL_SUCCESS)
        return 1;
    if(chl::chrono_monitor::initialize(conf.LOG_CONF.LOGTYPE,
                                       conf.LOG_CONF.LOGFILE,
                                       conf.LOG_CONF.LOGLEVEL,
                                       conf.LOG_CONF.LOGNAME,
                                       conf.LOG_CONF.LOGFILESIZE,
                                       conf.LOG_CONF.LOGFILENUM,
                                       conf.LOG_CONF.FLUSHLEVEL) != 0)
        return 1;
    auto publisher = rpc::LifecyclePublisher::Create(config);
    rpc::LifecycleRecord lifecycle;
    auto startup = rpc::RpcDeadline::fromStart(startupEntered, std::chrono::milliseconds(conf.RPC_POLICY.startup_ms));
    auto data = std::make_shared<KeeperData>(conf);
    auto generation = std::make_shared<rpc::NativeProcessGeneration>(3);
    std::vector<std::shared_ptr<rpc::RpcRuntime>> runtimes;
    std::vector<std::shared_ptr<rpc::RpcBulkOwner>> owners;
    for(int i = 0; i < 3; ++i)
    {
        auto runtime = std::make_shared<rpc::RpcRuntime>();
        auto owner = rpc::RpcBulkOwner::Prepare(runtime,
                                                conf.RPC_POLICY,
                                                i == 0,
                                                std::make_unique<rpc::NativeMidRuntime>(generation));
        owner->dependencies = data;
        runtimes.push_back(std::move(runtime));
        owners.push_back(std::move(owner));
    }
    {
        // retireServices drains the recording runtime last, so this callback
        // runs after all three MIDs are finalized and every status step on the
        // admin runtime has settled.
        std::weak_ptr<KeeperData> weak = data;
        owners[2]->afterCleanup = [publisher, generation, weak]
        {
            std::size_t handles = 0;
            if(auto alive = weak.lock())
                handles = alive->store.nativeHandleCount() + alive->extraction.nativeHandleCount();
            publisher->publishTerminal(rpc::LifecyclePublisher::record(rpc::ShutdownState::Stopped,
                                                                       chl::CL_SUCCESS,
                                                                       0,
                                                                       {},
                                                                       generation->snapshot(),
                                                                       handles));
        };
    }
    int startupStatus = rpc::initializeService(
            owners[0],
            *runtimes[0],
            {conf.EXTRACTION_MODULE_CONF.extraction_protocol, MARGO_CLIENT_MODE, 1},
            startup,
            data->initialized[0],
            data,
            [data, owner = owners[0]]
            {
                auto& chain = data->extraction.getExtractionChain();
                chain.activate(data->id.getRecordingServiceId(),
                               owner->engine.get(),
                               data->conf.EXTRACTION_MODULE_CONF);
                chain.set_watermark_reporter(data->admin);
                chain.attachRetentionStore(&data->retained);
                int result = data->extraction.initialize(data->conf.EXTRACTION_MODULE_CONF.extraction_stream_count);
                data->extractionReady = result == chl::CL_SUCCESS;
                return result;
            },
            &stop_requests);
    if(startupStatus == chl::CL_SUCCESS && conf.WAL_CONF.enabled)
    {
        // Recover the WAL before data collection starts, any service takes a
        // request or the keeper registers: the recovered events become sealed
        // chunks that extraction ships once it starts, and their WAL records
        // stay pending until the grapher confirms them. It needs Argobots, so
        // it runs after the first native instance is up. Recovery reads at
        // most WAL.max_bytes and counts against startup_ms.
        startupStatus = rpc::startLifecycleStep(
                *runtimes[0],
                startup,
                data->walStart,
                data,
                [data]
                {
                    std::string endpoint;
                    data->id.getRecordingServiceId().get_service_as_string(endpoint);
                    data->wal = start_keeper_wal(data->conf.WAL_CONF, endpoint, data->retained, data->store);
                    return data->wal ? int(chl::CL_SUCCESS) : int(chl::CL_ERR_UNKNOWN);
                });
        if(startupStatus == chl::CL_SUCCESS)
            startupStatus = rpc::waitLifecycleStep(data->walStart, startup, &stop_requests);
    }
    if(startupStatus == chl::CL_SUCCESS)
    {
        startupStatus = rpc::startLifecycleStep(*runtimes[0],
                                                startup,
                                                data->start,
                                                data,
                                                [data]
                                                {
                                                    data->started =
                                                            true; // Includes partial native startup if creation throws.
                                                    data->store.startDataCollection(3);
                                                    data->extraction.startExtraction();
                                                    return chl::CL_SUCCESS;
                                                });
        if(startupStatus == chl::CL_SUCCESS)
            startupStatus = rpc::waitLifecycleStep(data->start, startup, &stop_requests);
    }
    if(startupStatus == chl::CL_SUCCESS)
        startupStatus = rpc::initializeService(
                owners[1],
                *runtimes[1],
                {address(conf.DATA_STORE_ADMIN_SERVICE_CONF), MARGO_SERVER_MODE, 1},
                startup,
                data->initialized[1],
                data,
                [data, owner = owners[1]]
                {
                    return chl::DataStoreAdminService::CreateDataStoreAdminService(
                                   owner,
                                   data->conf.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID,
                                   data->store)
                                   ? chl::CL_SUCCESS
                                   : chl::CL_ERR_UNKNOWN;
                },
                &stop_requests);
    if(startupStatus == chl::CL_SUCCESS)
        startupStatus = rpc::initializeService(
                owners[2],
                *runtimes[2],
                {address(conf.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF),
                 MARGO_SERVER_MODE,
                 conf.KEEPER_RECORDING_SERVICE_CONF.INGESTION_THREAD_COUNT},
                startup,
                data->initialized[2],
                data,
                [data, owner = owners[2]]
                {
                    // The keeper side of record_event_durable. With no WAL it
                    // answers every durable write CL_ERR_DURABILITY_UNAVAILABLE
                    // and ingests nothing.
                    data->durableWriter =
                            std::make_unique<chl::KeeperDurableWriter>(data->wal.get(),
                                                                       data->ingestion,
                                                                       data->retained,
                                                                       data->store,
                                                                       data->conf.WAL_CONF.durableCapBytes());
                    return chl::KeeperRecordingService::CreateKeeperRecordingService(
                                   owner,
                                   data->conf.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID,
                                   data->ingestion,
                                   data->retained,
                                   [writer = data->durableWriter.get()](chl::ChronicleName const& chronicle,
                                                                        chl::StoryName const& story,
                                                                        chl::LogEvent const& event)
                                   { return writer->write(chronicle, story, event); })
                                   ? chl::CL_SUCCESS
                                   : chl::CL_ERR_UNKNOWN;
                },
                &stop_requests);
    if(startupStatus == chl::CL_SUCCESS)
    {
        data->registry.reset(chl::KeeperRegistryClient::CreateKeeperRegistryClient(
                owners[1],
                address(conf.VISOR_REGISTRY_SERVICE_CONF),
                conf.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID,
                startup));
        startupStatus = data->registry ? chl::CL_ERR_UNKNOWN : chl::CL_ERR_SHUTDOWN_INCOMPLETE;
        while(data->registry && !startup.expired() && !stop_requests)
        {
            startupStatus = data->registry->send_register_msg(chl::KeeperRegistrationMsg(data->id, data->admin),
                                                              startup,
                                                              &stop_requests);
            if(startupStatus == chl::CL_SUCCESS)
                break;
            std::this_thread::sleep_for(1ms);
        }
        if(startupStatus != chl::CL_SUCCESS)
            LOG_ERROR("[ChronoKeeper] Registration with ChronoVisor did not succeed: status {}", startupStatus);
    }
    int exitCode = startupStatus == chl::CL_SUCCESS ? 0 : 1;
    if(startupStatus == chl::CL_SUCCESS && !stop_requests)
    {
        publisher->request(rpc::LifecyclePublisher::record(rpc::ShutdownState::Running,
                                                           chl::CL_SUCCESS,
                                                           0,
                                                           startupEntered,
                                                           generation->snapshot(),
                                                           data->store.nativeHandleCount() +
                                                                   data->extraction.nativeHandleCount()));
        auto nextStats = rpc::RpcDeadline::Clock::now();
        while(!stop_requests)
        {
            publisher->pump(*runtimes[1], startup, data);
            if(rpc::RpcDeadline::Clock::now() >= nextStats)
            {
                data->registry->send_stats_msg(chl::KeeperStatsMsg(data->id));
                if(data->wal)
                {
                    chl::WalStats const stats = data->wal->stats();
                    LOG_INFO("[KeeperWal] stats groups={} events={} bytes={} max_events_per_group={} "
                             "pending_events={} pending_bytes={} segments={}",
                             stats.groups,
                             stats.events,
                             stats.bytes,
                             stats.max_events_per_group,
                             stats.pending_events,
                             stats.pending_bytes,
                             stats.segments);
                }
                nextStats = rpc::RpcDeadline::Clock::now() + 10s;
            }
            std::this_thread::sleep_for(100ms);
        }
    }
    auto stop = [&](rpc::RpcDeadline const& deadline)
    {
        owners[1]->quiesce();
        owners[2]->quiesce(); // Also closes providers whose construction is still running.
        // A timed-out constructor is still the same admitted action. Do not
        // touch borrowed fields or admit replacement construction until it settles.
        for(auto const& initialized: data->initialized)
            if(initialized)
            {
                rpc::waitLifecycleStep(initialized, deadline);
                if(!initialized->done.load(std::memory_order_acquire) || initialized->unsafe)
                    return false;
            }
        for(auto const& step: {data->walStart, data->start})
            if(step)
            {
                rpc::waitLifecycleStep(step, deadline);
                if(!step->done.load(std::memory_order_acquire) || step->unsafe)
                    return false;
            }
        while(!owners[1]->publicIdle() || !owners[2]->publicIdle())
        {
            if(deadline.expired())
                return false;
            std::this_thread::sleep_for(1ms);
        }
        auto phase = [&](std::shared_ptr<rpc::LifecycleStep>& record, std::function<int()> action)
        {
            while(!deadline.expired())
            {
                int admitted = rpc::startLifecycleStep(*runtimes[1], deadline, record, data, action);
                if(admitted == chl::CL_SUCCESS)
                    return rpc::waitLifecycleStep(record, deadline) == chl::CL_SUCCESS;
                if(admitted != chl::CL_ERR_RPC_CAPACITY)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        };
        if(data->started && !phase(data->stopCollection,
                                   [data]
                                   {
                                       data->store.shutdownDataCollection();
                                       return chl::CL_SUCCESS;
                                   }))
            return false;
        if(!phase(data->durable,
                  [data]
                  {
                      while(!data->retained.waitUntilDurable(
                              1s,
                              10ms,
                              std::chrono::seconds(data->conf.DATA_STORE_CONF.watermark_resend_timeout_secs)))
                      {}
                      return chl::CL_SUCCESS;
                  }))
            return false;
        if(data->extractionReady && !phase(data->stopExtraction,
                                           [data]
                                           {
                                               data->extraction.shutdownExtraction();
                                               return chl::CL_SUCCESS;
                                           }))
            return false;
        // Nothing reaches the WAL any more: every durable handler has answered
        // (public entries are idle), every chunk is archive-confirmed and
        // extraction is joined. Commit and answer what is queued, leave the
        // exit summary with the events the grapher did not confirm, which a
        // keeper started on the directory replays, and free the WAL while
        // Argobots is still up.
        if(data->walStart && !phase(data->walStop,
                                    [data]
                                    {
                                        if(data->wal)
                                        {
                                            stop_keeper_wal(*data->wal);
                                            data->retained.setWalObserver(nullptr);
                                            data->wal.reset();
                                        }
                                        return chl::CL_SUCCESS;
                                    }))
            return false;
        // Safe data is not sufficient if an original outgoing call is unknown.
        // Release only known snapshots, then prove all existing C2 slots idle.
        if(!data->unregistered)
        {
            owners[0]->retirementReady = true;
            if(!owners[0]->consumeKnownOutcomes(deadline) || !rpc::allControlIdle(*runtimes[0], 2, deadline) ||
               !rpc::allControlIdle(*runtimes[1], 2, deadline))
                return false;
            // The same irreversible drain stays ready across caller attempts.
            // Public entries are closed/idle, collectors and extractors joined,
            // all retained data is archive-confirmed and outgoing work settled.
            // Keep the actual admin MID until every managed publisher has
            // parked its original target sends and native cleanup.
            // Only a stop the deployment controller runs completes the
            // barrier. A keeper sent SIGTERM directly (a test, an operator,
            // a node agent) stops on its own, as v3.4.0 did: its data is
            // already confirmed or in its WAL's exit summary, and a grapher
            // still sending it reports gets a bounded failure.
            if(rpc::ManagedStopBarrier::StopRequested(config))
            {
                if(!rpc::allControlIdle(*runtimes[2], 2, deadline))
                    return false;
                auto native = generation->snapshot();
                if(data->publisherPermit && data->publisherPermit->done.load(std::memory_order_acquire) &&
                   !data->publisherPermit->unsafe && data->publisherPermit->status != chl::CL_SUCCESS)
                    data->publisherPermit.reset(); // Only a completed local proof observation is retried.
                if(!phase(data->publisherPermit,
                          [data, config, native, deadline]
                          {
                              std::string target;
                              data->admin.get_service_as_string(target);
                              target += "@" + std::to_string(data->admin.getProviderId());
                              if(!rpc::ManagedStopBarrier::PublishReady(config,
                                                                        data->conf.RECORDING_GROUP,
                                                                        native.generation,
                                                                        target,
                                                                        {true, true, true, true, true}))
                                  return int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                              while(!deadline.expired() && !rpc::ManagedStopBarrier::Permit(config,
                                                                                            data->conf.RECORDING_GROUP,
                                                                                            native.generation))
                                  std::this_thread::sleep_for(10ms);
                              return deadline.expired() ? int(chl::CL_ERR_SHUTDOWN_INCOMPLETE) : int(chl::CL_SUCCESS);
                          }))
                    return false;
            }
            while(data->registry && !deadline.expired())
            {
                int result = data->registry->send_unregister_msg(data->id, deadline);
                if(result == chl::CL_SUCCESS)
                    break;
                if(result == chl::CL_ERR_OUTCOME_UNCONFIRMED)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            if(deadline.expired())
                return false;
            data->unregistered = true;
            // The registry client's procedures reference the admin MID, which
            // is finalized only once nothing does. Main is its only user, and
            // the stats loop ended with the stop request.
            data->registry.reset();
        }
        return rpc::retireServices(owners, runtimes, deadline);
    };
    sig_atomic_t observed = 0;
    bool first = true;
    std::optional<rpc::RpcDeadline> previous;
    for(;;)
    {
        if(!first && observed == stop_requests)
        {
            // A status write refused for capacity may still be admitted, but
            // only before the deadline of the attempt that requested it.
            publisher->pump(*runtimes[1], *previous, data);
            std::this_thread::sleep_for(100ms);
            continue;
        }
        first = false;
        observed = stop_requests;
        auto entered = rpc::RpcDeadline::Clock::now();
        auto deadline = rpc::RpcDeadline::fromStart(entered, std::chrono::milliseconds(conf.RPC_POLICY.shutdown_ms));
        previous = deadline;
        auto attempt = ++lifecycle.attempts;
        publisher->beginAttempt(attempt, entered);
        lifecycle.quiesce();
        // Close public admission before any status write is requested. The
        // write itself runs on an admin worker under this attempt's deadline.
        owners[1]->quiesce();
        owners[2]->quiesce();
        publisher->request(rpc::LifecyclePublisher::record(rpc::ShutdownState::Quiescing,
                                                           chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                                                           attempt,
                                                           entered,
                                                           generation->snapshot(),
                                                           data->store.nativeHandleCount() +
                                                                   data->extraction.nativeHandleCount()));
        publisher->pump(*runtimes[1], deadline, data);
        // The Stopped record is written by the recording owner's cleanup
        // callback; a drained runtime alone does not prove it is in place.
        if(stop(deadline) && publisher->terminalPublished())
        {
            lifecycle.stopped();
            return exitCode;
        }
        lifecycle.draining();
        // An expired attempt admits no more work; it stays Draining in memory.
        if(!deadline.expired())
        {
            publisher->request(rpc::LifecyclePublisher::record(
                    rpc::ShutdownState::Draining,
                    chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                    attempt,
                    entered,
                    generation->snapshot(),
                    data->store.nativeHandleCount() + data->extraction.nativeHandleCount(),
                    std::nullopt,
                    [attempt] {
                        LOG_WARNING("[ChronoKeeper] stop attempt {} incomplete; retaining data and native owners",
                                    attempt);
                    }));
            publisher->pump(*runtimes[1], deadline, data);
        }
    }
}
