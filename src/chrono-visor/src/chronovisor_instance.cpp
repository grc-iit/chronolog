#include <signal.h>
#include <cstdint>
#include <iostream>
#include <cmd_arg_parse.h>
#include <KeeperRegistry.h>
#include <VisorClientPortal.h>
#include <ConfigurationManager.h>
#include <ChronoVisorConfiguration.h>
#include <private/LifecycleStep.h>
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
struct VisorData
{
    chl::VisorConfiguration conf;
    std::shared_ptr<rpc::NativeProcessGeneration> generation = std::make_shared<rpc::NativeProcessGeneration>(2);
    std::array<std::unique_ptr<rpc::NativeMidRuntime>, 2> mids;
    std::shared_ptr<chl::KeeperRegistry> registry;
    chl::VisorClientPortal portal;
    std::array<std::shared_ptr<rpc::LifecycleStep>, 2> initialized;
    std::shared_ptr<rpc::LifecycleStep> publicFile, finalPermit;
    std::atomic<bool> retirementReady{false}, cleanupComplete{false};
    bool publicSettled = false, publicProved = false, finalProved = false;
    VisorData(chl::VisorConfiguration const& conf, std::shared_ptr<rpc::RpcRuntime> const& actual)
        : conf(conf)
        , registry(chl::KeeperRegistry::CreateOwned(conf.RPC_POLICY, actual))
    {
        for(auto& mid: mids) mid = std::make_unique<rpc::NativeMidRuntime>(generation);
    }
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
    chl::VisorConfiguration conf;
    if(conf.parseJsonConf(manager.VISOR_JSON_CONF) != chl::CL_SUCCESS)
        return 1;
    if(chl::chrono_monitor::initialize(conf.VISOR_LOG_CONF.LOGTYPE,
                                       conf.VISOR_LOG_CONF.LOGFILE,
                                       conf.VISOR_LOG_CONF.LOGLEVEL,
                                       conf.VISOR_LOG_CONF.LOGNAME,
                                       conf.VISOR_LOG_CONF.LOGFILESIZE,
                                       conf.VISOR_LOG_CONF.LOGFILENUM,
                                       conf.VISOR_LOG_CONF.FLUSHLEVEL) != 0)
        return 1;
    auto publisher = rpc::LifecyclePublisher::Create(config);
    rpc::LifecycleRecord lifecycle;
    auto startup = rpc::RpcDeadline::fromStart(startupEntered, std::chrono::milliseconds(conf.RPC_POLICY.startup_ms));
    auto actual = std::make_shared<rpc::RpcRuntime>(32);
    auto data = std::make_shared<VisorData>(conf, actual);
    actual->retainOwner(data,
                        [data, publisher]
                        {
                            while(!data->retirementReady.load(std::memory_order_acquire) ||
                                  data->portal.core()->incomingRequests.load(std::memory_order_acquire) != 0)
                                std::this_thread::sleep_for(1ms);
                            // The actual Margo fences retain providers/metadata through queued
                            // handlers, copied requests and post-wrapper destruction. Both MIDs
                            // share this original C32 and the one existing ABI process generation.
                            data->mids[1]->cleanup();
                            data->portal.core()->releaseOwnedServices();
                            data->mids[0]->cleanup();
                            data->registry->releaseOwnedRegistryService();
                            data->cleanupComplete.store(true, std::memory_order_release);
                            // Every status step ran on this C32 and settled before
                            // this callback could start. Write the final snapshot.
                            publisher->publishTerminal(rpc::LifecyclePublisher::record(
                                    rpc::ShutdownState::Stopped,
                                    chl::CL_SUCCESS,
                                    0,
                                    {},
                                    data->generation->snapshot(),
                                    data->portal.core()->incomingRequests.load(std::memory_order_acquire)));
                        });
    auto initialize = [&](unsigned index, chl::RPCProviderConf const& spec, std::function<int()> setup)
    {
        int result = rpc::startLifecycleStep(*actual,
                                             startup,
                                             data->initialized[index],
                                             data,
                                             [data, index, spec, setup = std::move(setup)]
                                             {
                                                 int result = data->mids[index]->initialize(
                                                         {address(spec), MARGO_SERVER_MODE, 2});
                                                 return result == chl::CL_SUCCESS ? setup() : result;
                                             });
        return result == chl::CL_SUCCESS ? rpc::waitLifecycleStep(data->initialized[index], startup, &stop_requests)
                                         : result;
    };
    int startupStatus = initialize(0,
                                   conf.VISOR_KEEPER_REGISTRY_SERVICE_CONF,
                                   [data]
                                   {
                                       return data->registry->InitializeOwnedRegistryService(
                                               data->mids[0]->engine(),
                                               data->conf.VISOR_KEEPER_REGISTRY_SERVICE_CONF.SERVICE_PROVIDER_ID,
                                               data->conf.DELAYED_DATA_ADMIN_EXIT_IN_SECS);
                                   });
    if(startupStatus == chl::CL_SUCCESS)
        startupStatus = initialize(1,
                                   conf.VISOR_CLIENT_PORTAL_SERVICE_CONF,
                                   [data]
                                   {
                                       return data->portal.core()->StartOwnedServices(
                                               data->conf.VISOR_CLIENT_PORTAL_SERVICE_CONF,
                                               data->registry,
                                               data->mids[1]->engine());
                                   });
    int exitCode = startupStatus == chl::CL_SUCCESS ? 0 : 1;
    if(startupStatus == chl::CL_SUCCESS && !stop_requests)
    {
        publisher->request(rpc::LifecyclePublisher::record(rpc::ShutdownState::Running,
                                                           chl::CL_SUCCESS,
                                                           0,
                                                           startupEntered,
                                                           data->generation->snapshot(),
                                                           0));
        while(!stop_requests)
        {
            publisher->pump(*actual, startup, data);
            std::this_thread::sleep_for(100ms);
        }
    }
    // Why the current attempt returned early; logged by its Draining step.
    std::string incomplete;
    auto stop = [&](rpc::RpcDeadline const& deadline)
    {
        data->portal.ShutdownServices();
        data->registry->quiescePublic(); // Also closes a provider whose init is still admitted.
        for(auto const& initialized: data->initialized)
            if(initialized)
            {
                rpc::waitLifecycleStep(initialized, deadline);
                if(!initialized->done.load(std::memory_order_acquire) || initialized->unsafe)
                    return false;
            }
        if(!data->publicProved)
        {
            if(!data->publicSettled)
            {
                if(!data->portal.core()->quiesce(deadline) || !data->registry->quiescePublic(deadline) ||
                   !rpc::allControlIdle(*actual, 32, deadline))
                    return false;
                // Public admission stays closed. Original native/member work
                // is settled; do not reacquire all32 probes before file retries.
                data->publicSettled = true;
            }
            // No optional AdminWork follows this point. Managed Keeper TERM
            // performs its own original collector join; registry direct API
            // collection-phase semantics remain separate from this daemon stop.
            if(std::getenv("CHRONOLOG_DEPLOYMENT_ID"))
            {
                if(data->publicFile && data->publicFile->done.load(std::memory_order_acquire) &&
                   !data->publicFile->unsafe && data->publicFile->status != chl::CL_SUCCESS)
                    data->publicFile.reset();
                int result = rpc::startLifecycleStepWhenAvailable(
                        *actual,
                        deadline,
                        data->publicFile,
                        data,
                        [data, config]
                        {
                            auto const& spec = data->conf.VISOR_KEEPER_REGISTRY_SERVICE_CONF;
                            auto target = address(spec) + "@" + std::to_string(spec.SERVICE_PROVIDER_ID);
                            return rpc::ManagedStopBarrier::PublishPublicQuiesced(
                                           config,
                                           data->generation->snapshot().generation,
                                           target,
                                           {true, true, true})
                                           ? int(chl::CL_SUCCESS)
                                           : int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                        });
                if(result != chl::CL_SUCCESS)
                {
                    incomplete = "public proof admission incomplete: " + std::to_string(result);
                    return false;
                }
                int observed = rpc::waitLifecycleStep(data->publicFile, deadline);
                if(observed != chl::CL_SUCCESS)
                {
                    incomplete = "public proof publication incomplete: " + std::to_string(observed);
                    return false;
                }
            }
            data->publicProved = true;
        }
        while(!deadline.expired() && !data->registry->dependenciesUnregistered(deadline))
            std::this_thread::sleep_for(10ms);
        if(deadline.expired())
            return false;
        if(!data->finalProved)
        {
            if(std::getenv("CHRONOLOG_DEPLOYMENT_ID"))
            {
                while(!deadline.expired())
                {
                    if(data->finalPermit && data->finalPermit->done.load(std::memory_order_acquire) &&
                       !data->finalPermit->unsafe && data->finalPermit->status != chl::CL_SUCCESS)
                        data->finalPermit.reset();
                    int result = rpc::startLifecycleStep(*actual,
                                                         deadline,
                                                         data->finalPermit,
                                                         data,
                                                         [data, config]
                                                         {
                                                             return rpc::ManagedStopBarrier::PermitVisorRetirement(
                                                                            config,
                                                                            data->generation->snapshot().generation)
                                                                            ? int(chl::CL_SUCCESS)
                                                                            : int(chl::CL_ERR_SHUTDOWN_INCOMPLETE);
                                                         });
                    if(result == chl::CL_SUCCESS &&
                       rpc::waitLifecycleStep(data->finalPermit, deadline) == chl::CL_SUCCESS)
                    {
                        data->finalProved = true;
                        break;
                    }
                    if(result != chl::CL_SUCCESS && result != chl::CL_ERR_RPC_CAPACITY)
                        return false;
                    if(data->finalPermit && data->finalPermit->unsafe)
                        return false;
                    std::this_thread::sleep_for(10ms);
                }
            }
            else
                data->finalProved = data->registry->noRecordedDependencies(deadline);
            if(!data->finalProved)
                return false;
        }
        if(!data->retirementReady.load(std::memory_order_acquire))
        {
            if(!rpc::allControlIdle(*actual, 32, deadline))
                return false;
            data->retirementReady.store(true, std::memory_order_release);
        }
        return actual->drain(deadline) == chl::CL_SUCCESS && data->cleanupComplete.load(std::memory_order_acquire);
    };
    bool first = true;
    sig_atomic_t observed = 0;
    std::optional<rpc::RpcDeadline> previous;
    for(;;)
    {
        if(!first && observed == stop_requests)
        {
            // Admission of a refused status write ends with its attempt.
            publisher->pump(*actual, *previous, data);
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
        data->portal.ShutdownServices();
        data->registry->quiescePublic();
        publisher->request(
                rpc::LifecyclePublisher::record(rpc::ShutdownState::Quiescing,
                                                chl::CL_ERR_SHUTDOWN_INCOMPLETE,
                                                attempt,
                                                entered,
                                                data->generation->snapshot(),
                                                data->portal.core()->incomingRequests.load(std::memory_order_acquire)));
        publisher->pump(*actual, deadline, data);
        incomplete.clear();
        // The Stopped record is written by the C32 owner cleanup callback.
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
                    data->generation->snapshot(),
                    data->portal.core()->incomingRequests.load(std::memory_order_acquire),
                    std::nullopt,
                    [attempt, reason = incomplete]
                    {
                        if(!reason.empty())
                            LOG_WARNING("[ChronoVisor] {}", reason);
                        LOG_WARNING("[ChronoVisor] stop attempt {} incomplete; retaining both MIDs and original C32",
                                    attempt);
                    }));
            publisher->pump(*actual, deadline, data);
        }
    }
}
