#include <signal.h>
#include <cstdint>
#include <iostream>
#include <GrapherRecordingService.h>
#include <GrapherRegClient.h>
#include <GrapherDataStore.h>
#include <DataStoreAdminService.h>
#include <ConfigurationManager.h>
#include <StoryChunkExtractionModule.h>
#include <cmd_arg_parse.h>
#include <ChronoGrapherConfiguration.h>
#include <GrapherExtractionChain.h>
#include <private/ServiceLifecycle.h>
#include <private/LifecyclePublisher.h>
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
std::string address(chl::RPCProviderConf const& conf)
{
    return conf.PROTO_CONF + "://" + conf.IP + ":" + std::to_string(conf.BASE_PORT);
}
struct GrapherData
{
    chl::GrapherConfiguration conf;
    chl::GrapherIdCard id;
    chl::ServiceId admin;
    std::shared_ptr<chl::StoryWatermarkRegistry> watermarks = std::make_shared<chl::StoryWatermarkRegistry>();
    chl::StoryChunkExtractionModule<chl::ChronoGrapherExtractionChain> extraction;
    chl::ChunkIngestionQueue ingestion;
    chl::GrapherDataStore store;
    std::shared_ptr<chl::WatermarkReportPublisher> publisher;
    std::unique_ptr<chl::GrapherRegistryClient> registry;
    std::array<std::shared_ptr<rpc::LifecycleStep>, 2> initialized;
    std::shared_ptr<rpc::LifecycleStep> start, stopCollection, stopExtraction, parkPoll, terminalRead;
    rpc::StopDependencyEvidence terminal;
    bool started = false, extractionReady = false, unregistered = false;
    explicit GrapherData(chl::GrapherConfiguration const& c)
        : conf(c)
        , id(c.RECORDING_GROUP,
             chl::ServiceId(c.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF,
                            c.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP,
                            c.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT,
                            c.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID))
        , admin(c.DATA_STORE_ADMIN_SERVICE_CONF.PROTO_CONF,
                c.DATA_STORE_ADMIN_SERVICE_CONF.IP,
                c.DATA_STORE_ADMIN_SERVICE_CONF.BASE_PORT,
                c.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID)
        , extraction(c.EXTRACTION_MODULE_CONF.extraction_stream_count)
        , store(ingestion,
                extraction.getExtractionQueue(),
                c.DATA_STORE_CONF.max_story_chunk_size,
                c.DATA_STORE_CONF.story_chunk_duration_secs,
                c.DATA_STORE_CONF.acceptance_window_secs,
                c.DATA_STORE_CONF.inactive_story_delay_secs,
                &extraction.getExtractionChain(),
                watermarks.get())
    {}
};
} // namespace
int main(int argc, char** argv)
{
    auto startupEntered = rpc::RpcDeadline::Clock::now();
    signal(SIGTERM, request_stop);
    auto config = parse_conf_path_arg(argc, argv);
    if(config.empty())
        return 1;
    chl::ConfigurationManager manager(config);
    chl::GrapherConfiguration conf;
    if(conf.parseJsonConf(manager.GRAPHER_JSON_CONF) != chl::CL_SUCCESS)
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
    auto data = std::make_shared<GrapherData>(conf);
    auto generation = std::make_shared<rpc::NativeProcessGeneration>(2);
    std::vector<std::shared_ptr<rpc::RpcRuntime>> runtimes;
    std::vector<std::shared_ptr<rpc::RpcBulkOwner>> owners;
    for(int i = 0; i != 2; ++i)
    {
        auto runtime = std::make_shared<rpc::RpcRuntime>();
        auto owner = rpc::RpcBulkOwner::Prepare(runtime,
                                                conf.RPC_POLICY,
                                                false,
                                                std::make_unique<rpc::NativeMidRuntime>(generation));
        owner->dependencies = data;
        runtimes.push_back(std::move(runtime));
        owners.push_back(std::move(owner));
    }
    {
        // retireServices drains the recording runtime last, so this callback
        // runs after both MIDs are finalized and every status step on the
        // admin runtime has settled.
        std::weak_ptr<GrapherData> weak = data;
        owners[1]->afterCleanup = [publisher, generation, weak]
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
            {address(conf.DATA_STORE_ADMIN_SERVICE_CONF), MARGO_SERVER_MODE, 1},
            startup,
            data->initialized[0],
            data,
            [data, owner = owners[0]]
            {
                int result = data->extraction.getExtractionChain().activate(data->id.getRecordingServiceId(),
                                                                            data->conf.EXTRACTION_MODULE_CONF,
                                                                            data->watermarks.get(),
                                                                            std::to_string(data->conf.RECORDING_GROUP));
                if(result != chl::CL_SUCCESS)
                    return result;
                result = data->extraction.initialize(data->conf.EXTRACTION_MODULE_CONF.extraction_stream_count);
                data->extractionReady = result == chl::CL_SUCCESS;
                if(result != chl::CL_SUCCESS)
                    return result;
                data->publisher = std::make_shared<chl::WatermarkReportPublisher>(
                        owner,
                        data->watermarks,
                        data->conf.DATA_STORE_CONF.watermark_report_interval_secs);
                data->store.attachWatermarkPublisher(data->publisher.get());
                return chl::DataStoreAdminService::CreateDataStoreAdminService(
                               owner,
                               data->conf.DATA_STORE_ADMIN_SERVICE_CONF.SERVICE_PROVIDER_ID,
                               data->store)
                               ? int(chl::CL_SUCCESS)
                               : int(chl::CL_ERR_UNKNOWN);
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
                                                    data->started = true;
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
                {address(conf.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF),
                 MARGO_SERVER_MODE,
                 conf.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.INGESTION_THREAD_COUNT},
                startup,
                data->initialized[1],
                data,
                [data, owner = owners[1]]
                {
                    owner->incoming.pieceBytes = data->conf.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.TRANSFER_PIECE_BYTES;
                    data->watermarks->setDeliveryHold(
                            std::chrono::seconds(data->conf.DATA_STORE_CONF.duplicate_receipt_hold_secs));
                    return chl::GrapherRecordingService::CreateRecordingService(
                                   owner,
                                   data->conf.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.SERVICE_PROVIDER_ID,
                                   data->ingestion,
                                   data->publisher,
                                   data->watermarks.get())
                                   ? chl::CL_SUCCESS
                                   : chl::CL_ERR_UNKNOWN;
                },
                &stop_requests);
    if(startupStatus == chl::CL_SUCCESS)
    {
        data->registry.reset(
                chl::GrapherRegistryClient::CreateRegistryClient(owners[0],
                                                                 address(conf.VISOR_REGISTRY_SERVICE_CONF),
                                                                 conf.VISOR_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID,
                                                                 startup));
        startupStatus = data->registry ? chl::CL_ERR_UNKNOWN : chl::CL_ERR_SHUTDOWN_INCOMPLETE;
        while(data->registry && !startup.expired() && !stop_requests)
        {
            startupStatus = data->registry->send_register_msg(chl::GrapherRegistrationMsg(data->id, data->admin),
                                                              startup,
                                                              &stop_requests);
            if(startupStatus == chl::CL_SUCCESS)
                break;
            std::this_thread::sleep_for(1ms);
        }
    }
    auto pollPark = [&](std::optional<rpc::RpcDeadline> supplied = std::nullopt)
    {
        if(!data->publisher)
            return;
        auto deadline = rpc::RpcDeadline::after(std::chrono::milliseconds(conf.RPC_POLICY.attempt_ms));
        if(supplied && supplied->expiresAt() < deadline.expiresAt())
            deadline = *supplied;
        if(data->parkPoll && data->parkPoll->done.load(std::memory_order_acquire) && !data->parkPoll->unsafe)
            data->parkPoll.reset(); // A completed local observation; no native send is restarted.
        rpc::startLifecycleStep(
                *runtimes[0],
                deadline,
                data->parkPoll,
                data,
                [data, config, generation, deadline]
                {
                    auto batch = rpc::ManagedStopBarrier::LoadPublisher(config,
                                                                        data->conf.RECORDING_GROUP,
                                                                        generation->snapshot().generation);
                    if(!batch.valid())
                        return int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                    std::vector<rpc::PublisherParkProof> settled;
                    for(auto const& request: batch.requests())
                    {
                        if(data->publisher->park(request, deadline) == chl::CL_SUCCESS)
                            settled.push_back(request);
                        if(deadline.expired())
                            break;
                    }
                    return batch.acknowledge(settled) ? int(chl::CL_SUCCESS) : int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                });
    };
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
            publisher->pump(*runtimes[0], startup, data);
            pollPark();
            if(rpc::RpcDeadline::Clock::now() >= nextStats)
            {
                data->registry->send_stats_msg(chl::GrapherStatsMsg(data->id));
                nextStats = rpc::RpcDeadline::Clock::now() + 10s;
            }
            std::this_thread::sleep_for(100ms);
        }
    }
    auto stop = [&](rpc::RpcDeadline const& deadline, uint64_t attempt, uint64_t quiescing)
    {
        owners[0]->quiescePublic(); // Needed receipt/report paths remain open.
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
        // An earlier attempt passed every phase below and began retireServices,
        // which closes the admin runtime first: nothing can be admitted there
        // again. Continue the same retirement, which also lets the recording
        // owner's callback retry a failed Stopped write.
        if(data->unregistered)
            return rpc::retireServices(owners, runtimes, deadline);
        // StopDependencyEvidence requires this attempt's own Quiescing record.
        // Wait for that write without doing it here; a held write expires.
        if(!publisher->waitPublished(quiescing, *runtimes[0], deadline, data))
            return false;
        if(data->terminalRead && data->terminalRead->done.load(std::memory_order_acquire) &&
           !data->terminalRead->unsafe)
            data->terminalRead.reset();
        if(rpc::startLifecycleStep(
                   *runtimes[0],
                   deadline,
                   data->terminalRead,
                   data,
                   [data, config, attempt]
                   {
                       data->terminal = rpc::StopDependencyEvidence::Load(config, attempt, data->conf.RECORDING_GROUP);
                       return chl::CL_SUCCESS;
                   }) != chl::CL_SUCCESS ||
           rpc::waitLifecycleStep(data->terminalRead, deadline) != chl::CL_SUCCESS)
            return false;
        auto terminal = data->terminal;
        if(owners[1]->provider && !terminal.valid())
        {
            // A live/unknown Keeper may still need ingestion/archive/reporting.
            // Retain those dependencies until exact normal-Stopped+absence proof.
            pollPark(deadline);
            if(data->publisher)
                data->publisher->publishFinal(deadline);
            return false;
        }
        owners[1]->quiesce();
        while(!owners[0]->publicIdle() || owners[1]->incoming.used() != 0)
        {
            if(deadline.expired())
                return false;
            std::this_thread::sleep_for(1ms);
        }
        auto phase = [&](std::shared_ptr<rpc::LifecycleStep>& step, std::function<int()> action)
        {
            while(!deadline.expired())
            {
                int result = rpc::startLifecycleStep(*runtimes[0], deadline, step, data, action);
                if(result == chl::CL_SUCCESS)
                    return rpc::waitLifecycleStep(step, deadline) == chl::CL_SUCCESS;
                if(result != chl::CL_ERR_RPC_CAPACITY)
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
        if(data->extractionReady && !phase(data->stopExtraction,
                                           [data]
                                           {
                                               data->extraction.shutdownExtraction();
                                               return chl::CL_SUCCESS;
                                           }))
            return false;
        if(data->publisher)
        {
            while(!deadline.expired() && data->publisher->finish(deadline, terminal) != chl::CL_SUCCESS)
                std::this_thread::sleep_for(1ms);
            if(deadline.expired())
                return false;
        }
        if(!data->unregistered)
        {
            if(!rpc::allControlIdle(*runtimes[0], 2, deadline) || !rpc::allControlIdle(*runtimes[1], 2, deadline))
                return false;
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
            // Both runtimes are idle (above), so no report work uses it.
            if(data->publisher)
                data->publisher->releaseNative();
        }
        return rpc::retireServices(owners, runtimes, deadline);
    };
    bool first = true;
    sig_atomic_t observed = 0;
    std::optional<rpc::RpcDeadline> previous;
    for(;;)
    {
        if(!first && observed == stop_requests)
        {
            // Admission of a refused status write ends with its attempt.
            publisher->pump(*runtimes[0], *previous, data);
            pollPark();
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
        // Close public admission before any status write is requested.
        owners[0]->quiescePublic();
        auto quiescing = publisher->request(rpc::LifecyclePublisher::record(
                rpc::ShutdownState::Quiescing,
                chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                attempt,
                entered,
                generation->snapshot(),
                data->store.nativeHandleCount() + data->extraction.nativeHandleCount()));
        publisher->pump(*runtimes[0], deadline, data);
        // The Stopped record is written by the recording owner's cleanup callback.
        if(stop(deadline, attempt, quiescing) && publisher->terminalPublished())
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
                        LOG_WARNING("[ChronoGrapher] stop attempt {} incomplete; retaining data and native owners",
                                    attempt);
                    }));
            publisher->pump(*runtimes[0], deadline, data);
        }
    }
}
