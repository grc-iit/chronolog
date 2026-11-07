#include <signal.h>
#include <cstdint>
#include <PlayerRegClient.h>
#include <PlayerStoreAdminService.h>
#include <PlaybackService.h>
#include <PlaybackRpcOwner.h>
#include <StoryChunkConsumerService.h>
#include <ArchiveReadingAgent.h>
#include <ChronoPlayerConfiguration.h>
#include <ConfigurationManager.h>
#include <cmd_arg_parse.h>
#include <private/ServiceLifecycle.h>
#include <private/LifecycleStatus.h>
#include <private/ManagedStopBarrier.h>
namespace chl = chronolog;
namespace rpc = chronolog::rpc;
using namespace std::chrono_literals;
namespace
{
volatile sig_atomic_t stop_requests = 0;
void request_stop(int)
{
    if(stop_requests < SIG_ATOMIC_MAX)
        ++stop_requests;
}
std::string address(chl::RPCProviderConf const& c)
{
    return c.PROTO_CONF + "://" + c.IP + ":" + std::to_string(c.BASE_PORT);
}
chl::ServiceId service(chl::RPCProviderConf const& c)
{
    return chl::ServiceId(c.PROTO_CONF, c.IP, c.BASE_PORT, c.SERVICE_PROVIDER_ID);
}
struct PlayerData
{
    chl::PlayerConfiguration conf;
    chl::PlayerIdCard id;
    chl::ServiceId admin;
    chl::StoryChunkIngestionQueue ingestion;
    chl::PlayerDataStore store;
    chl::ArchiveReadingRequestQueue requests;
    chl::ArchiveReadingAgent reader;
    std::unique_ptr<chl::PlayerRegistryClient> registry;
    bool started = false, unregistered = false;
    std::atomic<bool> queryProved{false};
    std::shared_ptr<rpc::LifecycleStep> queryPoll, ingestionPermit;
    std::shared_ptr<rpc::LifecycleStep> playback, recording, administration, stopStore, stopReader;
    explicit PlayerData(chl::PlayerConfiguration const& c)
        : conf(c)
        , id(c.RECORDING_GROUP, service(c.PLAYBACK_SERVICE_CONF.RPC_CONF))
        , admin(service(c.DATA_STORE_ADMIN_SERVICE_CONF))
        , store(ingestion,
                c.DATA_STORE_CONF.max_story_chunk_size,
                c.DATA_STORE_CONF.story_chunk_duration_secs,
                c.DATA_STORE_CONF.acceptance_window_secs,
                c.DATA_STORE_CONF.inactive_story_delay_secs)
        , reader(requests, c.READER_CONF.story_files_dir)
    {}
};
} // namespace
int main(int argc, char** argv)
{
    auto entered = rpc::RpcDeadline::Clock::now();
    signal(SIGTERM, request_stop);
    auto config = parse_conf_path_arg(argc, argv);
    if(config.empty())
        return 1;
    chl::ConfigurationManager manager(config);
    chl::PlayerConfiguration conf;
    if(conf.parseJsonConf(manager.PLAYER_JSON_CONF) != chl::CL_SUCCESS)
        return 1;
    if(chl::chrono_monitor::initialize(conf.LOG_CONF.LOGTYPE,
                                       conf.LOG_CONF.LOGFILE,
                                       conf.LOG_CONF.LOGLEVEL,
                                       conf.LOG_CONF.LOGNAME,
                                       conf.LOG_CONF.LOGFILESIZE,
                                       conf.LOG_CONF.LOGFILENUM,
                                       conf.LOG_CONF.FLUSHLEVEL) != 0)
        return 1;
    rpc::LifecycleStatus statusFile(config);
    rpc::LifecycleRecord lifecycle;
    auto deadline = rpc::RpcDeadline::fromStart(entered, std::chrono::milliseconds(conf.RPC_POLICY.startup_ms));
    auto data = std::make_shared<PlayerData>(conf);
    auto generation = std::make_shared<rpc::NativeProcessGeneration>(3);
    auto playbackRuntime = std::make_shared<rpc::RpcRuntime>(6);
    auto recordingRuntime = std::make_shared<rpc::RpcRuntime>();
    auto adminRuntime = std::make_shared<rpc::RpcRuntime>();
    auto playback = chl::PlaybackRpcOwner::Prepare(playbackRuntime,
                                                   conf.RPC_POLICY,
                                                   std::make_unique<rpc::NativeMidRuntime>(generation));
    auto recording = rpc::RpcBulkOwner::Prepare(recordingRuntime,
                                                conf.RPC_POLICY,
                                                false,
                                                std::make_unique<rpc::NativeMidRuntime>(generation));
    auto administration = rpc::RpcBulkOwner::Prepare(adminRuntime,
                                                     conf.RPC_POLICY,
                                                     false,
                                                     std::make_unique<rpc::NativeMidRuntime>(generation));
    playback->dependencies = data;
    recording->dependencies = data;
    administration->dependencies = data;
    int startup = rpc::startLifecycleStep(*playbackRuntime,
                                          deadline,
                                          data->playback,
                                          data,
                                          [data, playback]() -> int
                                          {
                                              auto const& conf = data->conf;
                                              int status = playback->initialize(
                                                      {address(conf.PLAYBACK_SERVICE_CONF.RPC_CONF),
                                                       MARGO_SERVER_MODE,
                                                       conf.PLAYBACK_SERVICE_CONF.INGESTION_THREAD_COUNT});
                                              if(status != chl::CL_SUCCESS)
                                                  return status;
                                              data->started = true;
                                              data->store.startDataCollection(3);
                                              data->reader.startArchiveReading(1);
                                              return chl::PlaybackService::CreatePlaybackService(
                                                             playback,
                                                             conf.PLAYBACK_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID,
                                                             data->store,
                                                             data->requests)
                                                             ? chl::CL_SUCCESS
                                                             : chl::CL_ERR_UNKNOWN;
                                          });
    if(startup == chl::CL_SUCCESS)
        startup = rpc::waitLifecycleStep(data->playback, deadline, &stop_requests);
    if(startup == chl::CL_SUCCESS)
        startup = rpc::initializeService(
                recording,
                *recordingRuntime,
                {address(conf.RECORDING_SERVICE_CONF.RPC_CONF),
                 MARGO_SERVER_MODE,
                 conf.RECORDING_SERVICE_CONF.INGESTION_THREAD_COUNT},
                deadline,
                data->recording,
                data,
                [data, recording]
                {
                    return chl::StoryChunkConsumerService::CreateChunkConsumerService(
                                   recording,
                                   data->conf.RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID,
                                   data->ingestion)
                                   ? chl::CL_SUCCESS
                                   : chl::CL_ERR_UNKNOWN;
                },
                &stop_requests);
    if(startup == chl::CL_SUCCESS)
        startup = rpc::initializeService(
                administration,
                *adminRuntime,
                {address(conf.DATA_STORE_ADMIN_SERVICE_CONF), MARGO_SERVER_MODE, 1},
                deadline,
                data->administration,
                data,
                [data, administration]
                {
                    return chl::PlayerStoreAdminService::CreatePlayerStoreAdminService(
                                   administration,
                                   data->conf.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID,
                                   data->store)
                                   ? chl::CL_SUCCESS
                                   : chl::CL_ERR_UNKNOWN;
                },
                &stop_requests);
    if(startup == chl::CL_SUCCESS)
    {
        data->registry.reset(
                chl::PlayerRegistryClient::CreateRegistryClient(administration,
                                                                address(conf.VISOR_REGISTRY_SERVICE_CONF),
                                                                conf.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID,
                                                                deadline));
        startup = data->registry ? chl::CL_ERR_UNKNOWN : chl::CL_ERR_SHUTDOWN_INCOMPLETE;
        while(data->registry && !deadline.expired() && !stop_requests)
        {
            startup = data->registry->send_register_msg(chl::PlayerRegistrationMsg(data->id, data->admin),
                                                        deadline,
                                                        &stop_requests);
            if(startup == chl::CL_SUCCESS)
                break;
            std::this_thread::sleep_for(1ms);
        }
    }
    auto pollQueries = [&]
    {
        if(!std::getenv("CHRONOLOG_DEPLOYMENT_ID") || data->queryProved.load(std::memory_order_acquire))
            return;
        auto local = rpc::RpcDeadline::after(std::chrono::milliseconds(conf.RPC_POLICY.attempt_ms));
        if(data->queryPoll && data->queryPoll->done.load(std::memory_order_acquire) && !data->queryPoll->unsafe)
            data->queryPoll.reset(); // Only a completed local observation is retried.
        rpc::startLifecycleStep(
                *adminRuntime,
                local,
                data->queryPoll,
                data,
                [data, playback, generation, config, local]
                {
                    auto native = generation->snapshot();
                    if(!rpc::ManagedStopBarrier::QueriesRequested(config,
                                                                  data->conf.RECORDING_GROUP,
                                                                  native.generation) ||
                       !playback->quiesceQueries(local))
                        return int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                    auto target = address(data->conf.PLAYBACK_SERVICE_CONF.RPC_CONF) + "@" +
                                  std::to_string(data->conf.PLAYBACK_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID);
                    if(!rpc::ManagedStopBarrier::PublishQueriesQuiesced(
                               config,
                               data->conf.RECORDING_GROUP,
                               native.generation,
                               target,
                               {playback->closing(), playback->queriesSettled.load(), true}))
                        return int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                    data->queryProved.store(true, std::memory_order_release);
                    return int(chl::CL_SUCCESS);
                });
    };
    int exitCode = startup == chl::CL_SUCCESS ? 0 : 1;
    if(startup == chl::CL_SUCCESS && !stop_requests)
    {
        auto native = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Running,
                           chl::CL_SUCCESS,
                           0,
                           entered,
                           &native,
                           data->store.nativeHandleCount() + data->reader.nativeHandleCount());
        auto nextStats = rpc::RpcDeadline::Clock::now();
        while(!stop_requests)
        {
            pollQueries();
            if(rpc::RpcDeadline::Clock::now() >= nextStats)
            {
                data->registry->send_stats_msg(chl::PlayerStatsMsg(data->id));
                nextStats = rpc::RpcDeadline::Clock::now() + 10s;
            }
            std::this_thread::sleep_for(100ms);
        }
    }
    auto stop = [&](rpc::RpcDeadline const& stopDeadline)
    {
        playback->quiesce();
        for(auto const& init: {data->playback, data->recording, data->administration})
            if(init)
            {
                rpc::waitLifecycleStep(init, stopDeadline);
                if(!init->done.load(std::memory_order_acquire) || init->unsafe)
                    return false;
            }
        // Keep readers/stores alive while an admitted coordinator or copied
        // response still needs them. Native unknown stays in its original C6.
        if(!data->unregistered && !playback->quiesceQueries(stopDeadline))
            return false;
        if(!data->unregistered && std::getenv("CHRONOLOG_DEPLOYMENT_ID"))
        {
            if(data->ingestionPermit && data->ingestionPermit->done.load(std::memory_order_acquire) &&
               !data->ingestionPermit->unsafe && data->ingestionPermit->status != chl::CL_SUCCESS)
                data->ingestionPermit.reset(); // Only a completed local observation is retried.
            int admitted = rpc::startLifecycleStepWhenAvailable(
                    *adminRuntime,
                    stopDeadline,
                    data->ingestionPermit,
                    data,
                    [data, config, generation]
                    {
                        return rpc::ManagedStopBarrier::PermitPlayerIngestionStop(config,
                                                                                  data->conf.RECORDING_GROUP,
                                                                                  generation->snapshot().generation)
                                       ? int(chl::CL_SUCCESS)
                                       : int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                    });
            if(admitted != chl::CL_SUCCESS ||
               rpc::waitLifecycleStep(data->ingestionPermit, stopDeadline) != chl::CL_SUCCESS)
                return false;
        }
        recording->quiesce();
        administration->quiesce();
        while(!playback->entriesIdle() || !administration->publicIdle() || recording->incoming.used())
        {
            if(stopDeadline.expired())
                return false;
            std::this_thread::sleep_for(1ms);
        }
        auto phase = [&](std::shared_ptr<rpc::LifecycleStep>& step, std::function<int()> action)
        {
            while(!stopDeadline.expired())
            {
                int status = rpc::startLifecycleStep(*adminRuntime, stopDeadline, step, data, action);
                if(status == chl::CL_SUCCESS)
                    return rpc::waitLifecycleStep(step, stopDeadline) == chl::CL_SUCCESS;
                if(status != chl::CL_ERR_RPC_CAPACITY)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        };
        if(data->started && (!phase(data->stopReader,
                                    [data]
                                    {
                                        data->reader.shutdownArchiveReading();
                                        return chl::CL_SUCCESS;
                                    }) ||
                             !phase(data->stopStore,
                                    [data]
                                    {
                                        data->store.shutdownDataCollection();
                                        return chl::CL_SUCCESS;
                                    })))
            return false;
        if(!data->unregistered)
        {
            if(!rpc::allControlIdle(*recordingRuntime, 2, stopDeadline) ||
               !rpc::allControlIdle(*adminRuntime, 2, stopDeadline))
                return false;
            while(data->registry && !stopDeadline.expired())
            {
                int result = data->registry->send_unregister_msg(data->id, stopDeadline);
                if(result == chl::CL_SUCCESS)
                    break;
                if(result == chl::CL_ERR_OUTCOME_UNCONFIRMED)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            if(stopDeadline.expired())
                return false;
            data->unregistered = true;
        }
        playback->retirementReady = true;
        if(!rpc::retireServices({recording, administration}, {recordingRuntime, adminRuntime}, stopDeadline))
            return false;
        return playbackRuntime->drain(stopDeadline) == chl::CL_SUCCESS;
    };
    sig_atomic_t observed = 0;
    bool first = true;
    for(;;)
    {
        if(!first && observed == stop_requests)
        {
            pollQueries();
            std::this_thread::sleep_for(100ms);
            continue;
        }
        first = false;
        observed = stop_requests;
        auto start = rpc::RpcDeadline::Clock::now();
        auto stopDeadline = rpc::RpcDeadline::fromStart(start, std::chrono::milliseconds(conf.RPC_POLICY.shutdown_ms));
        auto attempt = ++lifecycle.attempts;
        lifecycle.quiesce();
        auto retainedProof = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Quiescing,
                           chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                           attempt,
                           start,
                           &retainedProof,
                           data->store.nativeHandleCount() + data->reader.nativeHandleCount());
        if(stop(stopDeadline))
        {
            auto nativeProof = generation->snapshot();
            auto applicationHandles = data->store.nativeHandleCount() + data->reader.nativeHandleCount();
            lifecycle.stopped();
            if(statusFile.publish(rpc::ShutdownState::Stopped,
                                  chl::CL_SUCCESS,
                                  attempt,
                                  start,
                                  &nativeProof,
                                  applicationHandles))
                return exitCode;
        }
        lifecycle.draining();
        retainedProof = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Draining,
                           chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                           attempt,
                           start,
                           &retainedProof,
                           data->store.nativeHandleCount() + data->reader.nativeHandleCount());
        LOG_WARNING("[ChronoPlayer] stop attempt {} incomplete; retaining native work and data", attempt);
    }
}
