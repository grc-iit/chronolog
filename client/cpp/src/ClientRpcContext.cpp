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
namespace
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
}
ClientRpcProcedures::ClientRpcProcedures(thallium::engine& engine)
    : registrations_(engine)
{
    for(size_t i = 0; i < calls.size(); ++i) calls[i] = engine.define(names[i]);
}
int ClientRpcProcedures::provision(Name name, uint16_t provider, rpc::RpcDeadline const& deadline)
{
    auto index = static_cast<size_t>(name);
    return index < Count ? registrations_.provision(names[index], provider, deadline) : CL_ERR_INVALID_ARG;
}
// Margo owns these once-defined registrations through its final incoming fence.
// Descriptor destruction is std-only even when a reader still shares the cache.
ClientRpcProcedures::~ClientRpcProcedures() = default;
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
    {
        try
        {
            reader = ClientQueryService::CreateClientQueryService(
                    *engine,
                    ServiceId(queryConf.PROTO_CONF, queryIp.ip, queryConf.PORT, queryConf.PROVIDER_ID),
                    procedures,
                    shared_from_this());
            if(!reader)
            {
                initializeStatus_ = CL_ERR_UNKNOWN;
                return initializeStatus_;
            }
        }
        catch(...)
        {
            // A nonnull provider root after partial registration is not a
            // successful initialization. Never retry into a partial reader.
            initializeStatus_ = CL_ERR_UNKNOWN;
            throw;
        }
    }
    auto address = portalConf.PROTO_CONF + "://" + portalConf.IP + ":" + std::to_string(portalConf.PORT);
    visor = std::make_unique<RpcVisorClient>(*engine, address, portalConf.PROVIDER_ID, procedures);
    if(beforeInitializeUnlock_)
        beforeInitializeUnlock_();
    return deadline.expired() ? CL_ERR_DEADLINE_EXCEEDED : CL_SUCCESS;
}

std::pair<int, ClientQueryService*> ClientRpcContext::admitReader(rpc::RpcDeadline const& deadline)
{
    if(deadline.expired())
        return {CL_ERR_DEADLINE_EXCEEDED, nullptr};
    std::unique_lock<std::timed_mutex> lock(initializeMutex_, std::defer_lock);
    if(!lock.try_lock_until(deadline.expiresAt()) || deadline.expired())
        return {CL_ERR_DEADLINE_EXCEEDED, nullptr};
    return {reader ? CL_SUCCESS : CL_ERR_NO_PLAYERS, reader};
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
    if(mutations.unresolved())
        throw std::runtime_error("Client metadata outcome remains unresolved");
    for(;;)
    {
        // Bounded public expiry can leave a consumed record behind a contended
        // map lock. Prune before its retained lease is used as an idle test.
        bool pruned = !reader || reader->pruneConsumedQueries();
        if(pruned && nativeUses_.load(std::memory_order_acquire) == Closed && incomingResponses.used() == 0 &&
           replayQueries.used() == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    storyteller.reset();
    visor.reset();
    // Copied requests/bulk/native leases are settled. Keep the actual reader,
    // procedure cache and engine wrapper rooted through Margo's final pending-
    // handler fence, including rejected entries and post-wrapper destruction.
    native_.cleanup();
    delete reader;
    reader = nullptr;
    procedures.reset();
    engine.reset();
    cleanupComplete.store(true, std::memory_order_release);
}
} // namespace chronolog
