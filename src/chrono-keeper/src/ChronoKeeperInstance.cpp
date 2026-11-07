#include <signal.h>
#include <cstdint>
#include <iostream>
#include <KeeperRecordingService.h>
#include <KeeperRegClient.h>
#include <KeeperDataStore.h>
#include <DataStoreAdminService.h>
#include <KeeperExtractionChain.h>
#include <StoryChunkExtractionModule.h>
#include <ConfigurationManager.h>
#include <ChronoKeeperConfiguration.h>
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
struct KeeperData
{
    chl::KeeperConfiguration conf;
    chl::KeeperIdCard id;
    chl::ServiceId admin;
    chl::StoryChunkExtractionModule<chl::ChronoKeeperExtractionChain> extraction;
    chl::KeeperChunkRetentionStore retained;
    chl::IngestionQueue ingestion;
    chl::KeeperDataStore store;
    std::unique_ptr<chl::KeeperRegistryClient> registry;
    bool started = false, extractionReady = false, unregistered = false;
    std::array<std::shared_ptr<rpc::LifecycleStep>, 3> initialized;
    std::shared_ptr<rpc::LifecycleStep> start, stopCollection, durable, stopExtraction, publisherPermit;
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
    rpc::LifecycleStatus statusFile(config);
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
                    return chl::KeeperRecordingService::CreateKeeperRecordingService(
                                   owner,
                                   data->conf.KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID,
                                   data->ingestion,
                                   data->retained)
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
    }
    int exitCode = startupStatus == chl::CL_SUCCESS ? 0 : 1;
    if(startupStatus == chl::CL_SUCCESS && !stop_requests)
    {
        auto runningProof = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Running,
                           chl::CL_SUCCESS,
                           0,
                           startupEntered,
                           &runningProof,
                           data->store.nativeHandleCount() + data->extraction.nativeHandleCount());
        auto nextStats = rpc::RpcDeadline::Clock::now();
        while(!stop_requests)
        {
            if(rpc::RpcDeadline::Clock::now() >= nextStats)
            {
                data->registry->send_stats_msg(chl::KeeperStatsMsg(data->id));
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
        if(data->start)
        {
            rpc::waitLifecycleStep(data->start, deadline);
            if(!data->start->done.load(std::memory_order_acquire) || data->start->unsafe)
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
            bool managed = std::getenv("CHRONOLOG_DEPLOYMENT_ID") != nullptr;
            if(managed || owners[2]->writeAdmitted.load(std::memory_order_acquire))
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
        }
        return rpc::retireServices(owners, runtimes, deadline);
    };
    sig_atomic_t observed = 0;
    bool first = true;
    for(;;)
    {
        if(!first && observed == stop_requests)
        {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        first = false;
        observed = stop_requests;
        auto entered = rpc::RpcDeadline::Clock::now();
        auto deadline = rpc::RpcDeadline::fromStart(entered, std::chrono::milliseconds(conf.RPC_POLICY.shutdown_ms));
        auto attempt = ++lifecycle.attempts;
        lifecycle.quiesce();
        auto drainingProof = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Quiescing,
                           chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                           attempt,
                           entered,
                           &drainingProof,
                           data->store.nativeHandleCount() + data->extraction.nativeHandleCount());
        if(stop(deadline))
        {
            auto nativeProof = generation->snapshot();
            auto applicationHandles = data->store.nativeHandleCount() + data->extraction.nativeHandleCount();
            lifecycle.stopped();
            if(statusFile.publish(rpc::ShutdownState::Stopped,
                                  chl::CL_SUCCESS,
                                  attempt,
                                  entered,
                                  &nativeProof,
                                  applicationHandles))
                return exitCode;
        }
        lifecycle.draining();
        drainingProof = generation->snapshot();
        statusFile.publish(rpc::ShutdownState::Draining,
                           chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                           attempt,
                           entered,
                           &drainingProof,
                           data->store.nativeHandleCount() + data->extraction.nativeHandleCount());
        LOG_WARNING("[ChronoKeeper] stop attempt {} incomplete; retaining data and native owners", attempt);
    }
}
