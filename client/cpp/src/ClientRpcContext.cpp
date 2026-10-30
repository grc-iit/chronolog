#include "ClientRpcContext.h"
#include "ClientLoginName.h"
#include "ClientQueryService.h"
#include "QueryServiceAddress.h"
#include "StorytellerClient.h"
#include "rpcVisorClient.h"
#include <margo.h>
#include <unistd.h>
#include <thread>
#include <chrono_monitor.h>
namespace chronolog
{
ClientRpcProcedures::ClientRpcProcedures(thallium::engine& engine)
{
    constexpr char const* names[] = {"Connect",
                                     "Disconnect",
                                     "CreateChronicle",
                                     "DestroyChronicle",
                                     "AcquireStory",
                                     "ReleaseStory",
                                     "DestroyStory",
                                     "ShowChronicles",
                                     "ShowStories",
                                     "record_event",
                                     "tail_get_sequences",
                                     "tail_get_events",
                                     "playback_service_available",
                                     "story_playback_request"};
    for(size_t i = 0; i < calls.size(); ++i) calls[i] = engine.define(names[i]);
}
ClientRpcProcedures::~ClientRpcProcedures()
{
    for(auto& call: calls) call.deregister();
}
ClientRpcContext::ClientRpcContext(ClientPortalServiceConf const& portal,
                                   bool mode,
                                   ClientQueryServiceConf const& query,
                                   ChronologTimer::RawSource source)
    : policy(std::make_shared<RpcPolicyContext>(portal.RPC_POLICY))
    , timer(std::move(source))
    , portalConf(portal)
    , queryConf(query)
    , readerMode(mode)
{}
int ClientRpcContext::initialize(rpc::RpcDeadline const& deadline)
{
    std::unique_lock<std::timed_mutex> lock(initializeMutex_, std::defer_lock);
    if(!lock.try_lock_until(deadline.expiresAt()))
        return CL_ERR_DEADLINE_EXCEEDED;
    struct PublishSettlement
    {
        std::atomic<bool>& settled;
        ~PublishSettlement() { settled.store(true, std::memory_order_release); }
    } publication{initializationSettled};
    initializationSettled.store(false, std::memory_order_release);
    if(visor || initializeStatus_ != CL_SUCCESS)
        return initializeStatus_;
    if(deadline.expired())
        return CL_ERR_DEADLINE_EXCEEDED;
    chrono_monitor::initialize("file",
                               "/tmp/chrono_client.log",
                               LogLevel::info,
                               "chrono_client",
                               1024000,
                               3,
                               LogLevel::warn);
    auto towards = address_towards_remote_visor(portalConf.IP);
    QueryServiceIp queryIp;
    if(readerMode)
        queryIp = query_service_ip(queryConf.IP, towards);
    euid = geteuid();
    identity.ip = client_host_ipv4(readerMode ? std::optional<std::string>(queryIp.ip) : std::nullopt, towards);
    identity.port = readerMode ? queryConf.PORT : uint16_t{0};
    identity.instance = static_cast<uint16_t>(getpid() & 0xffffu);
    if(deadline.expired())
        return CL_ERR_DEADLINE_EXCEEDED;
    if(!engine)
    {
        auto address = readerMode ? queryConf.PROTO_CONF + "://" + queryIp.ip + ":" + std::to_string(queryConf.PORT)
                                  : portalConf.PROTO_CONF;
        initializeStatus_ = native_.initialize(std::move(address), readerMode ? MARGO_SERVER_MODE : MARGO_CLIENT_MODE);
        if(initializeStatus_ != CL_SUCCESS)
        {
            return initializeStatus_;
        }
        engine = native_.engine();
    }
    if(!procedures)
        procedures = std::make_shared<ClientRpcProcedures>(*engine);
    if(readerMode && !reader)
        reader = ClientQueryService::CreateClientQueryService(
                *engine,
                ServiceId(queryConf.PROTO_CONF, queryIp.ip, queryConf.PORT, queryConf.PROVIDER_ID),
                procedures,
                shared_from_this());
    auto address = portalConf.PROTO_CONF + "://" + portalConf.IP + ":" + std::to_string(portalConf.PORT);
    visor = std::make_unique<RpcVisorClient>(*engine, address, portalConf.PROVIDER_ID, procedures);
    return deadline.expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_SUCCESS;
}

ClientRpcContext::~ClientRpcContext() = default;
bool ClientRpcContext::retainFacade() noexcept
{
    auto observed = facades_.load(std::memory_order_acquire);
    while(!(observed & Closed) && observed != Closed - 1)
        if(facades_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
            return true;
    return false;
}
void ClientRpcContext::releaseFacade() noexcept
{
    auto observed = facades_.load(std::memory_order_acquire);
    for(;;)
    {
        unsigned next = observed == 1 ? Closed : observed - 1;
        if(facades_.compare_exchange_weak(observed, next, std::memory_order_acq_rel))
        {
            if(next == Closed)
                nativeUses_.fetch_or(Closed, std::memory_order_acq_rel);
            return;
        }
    }
}
bool ClientRpcContext::closing() const noexcept { return (facades_.load(std::memory_order_acquire) & Closed) != 0; }
ClientRpcContext::NativeLease::NativeLease(std::shared_ptr<ClientRpcContext> owner)
    : owner_(std::move(owner))
{}
ClientRpcContext::NativeLease::NativeLease(NativeLease&& other) noexcept
    : owner_(std::move(other.owner_))
{}
ClientRpcContext::NativeLease::~NativeLease()
{
    if(owner_)
        owner_->nativeUses_.fetch_sub(1, std::memory_order_acq_rel);
}
std::optional<ClientRpcContext::NativeLease> ClientRpcContext::acquireNative()
{
    auto observed = nativeUses_.load(std::memory_order_acquire);
    while(!(observed & Closed) && observed != Closed - 1)
        if(nativeUses_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
            return NativeLease(shared_from_this());
    return std::nullopt;
}
void ClientRpcContext::cleanup()
{
    nativeUses_.fetch_or(Closed, std::memory_order_acq_rel);
    if(storyteller)
        storyteller->quiesceNativeEntries();
    while(nativeUses_.load(std::memory_order_acquire) != Closed)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // B09/B12 must establish incoming provider quiescence before reader native
    // cleanup can be proved safe. Keep this existing cleanup slot quarantined.
    if(mutations.unresolved())
        throw std::runtime_error("Client metadata outcome remains unresolved");
    if(reader)
        throw std::runtime_error("Client reader incoming work is not proven settled");
    storyteller.reset();
    visor.reset();
    procedures.reset();
    engine.reset();
    native_.cleanup();
    cleanupComplete.store(true, std::memory_order_release);
}
} // namespace chronolog
