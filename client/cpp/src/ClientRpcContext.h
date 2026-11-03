#ifndef CHRONOLOG_CLIENT_RPC_CONTEXT_H
#define CHRONOLOG_CLIENT_RPC_CONTEXT_H
#include "ChronologTimer.h"
#include "ClientNativeRuntime.h"
#include "MetadataOperationState.h"
#include "RpcPolicyContext.h"
#include <RpcRuntime.h>
#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <thallium.hpp>
#include <array>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
namespace chronolog
{
class RpcVisorClient;
class StorytellerClient;
class ClientQueryService;
struct ClientRpcProcedures
{
    enum Name
    {
        Connect,
        Disconnect,
        CreateChronicle,
        DestroyChronicle,
        AcquireStory,
        ReleaseStory,
        DestroyStory,
        ShowChronicles,
        ShowStories,
        RecordEvent,
        TailSequences,
        TailEvents,
        PlaybackAvailable,
        PlaybackRequest,
        Count
    };
    explicit ClientRpcProcedures(thallium::engine&);
    ~ClientRpcProcedures();
    std::array<thallium::remote_procedure, Count> calls;
};
// Actual native lifetime, retained by the existing runtime owner independently
// of lightweight Client wrappers. No runtime backlink or engine-copy finalizer.
class ClientRpcContext: public std::enable_shared_from_this<ClientRpcContext>
{
public:
    ClientRpcContext(ClientPortalServiceConf const&,
                     bool reader,
                     ClientQueryServiceConf const&,
                     ChronologTimer::RawSource = {});
    ~ClientRpcContext();
    bool retainFacade() noexcept;
    void releaseFacade() noexcept;
    bool closing() const noexcept;
    void cleanup();
    std::weak_ptr<rpc::RpcRuntime> runtime;
    std::atomic<bool> cleanupComplete{false};
    class NativeLease
    {
    public:
        NativeLease(NativeLease const&) = delete;
        NativeLease(NativeLease&&) noexcept;
        ~NativeLease();

    private:
        friend class ClientRpcContext;
        explicit NativeLease(std::shared_ptr<ClientRpcContext>);
        std::shared_ptr<ClientRpcContext> owner_;
    };
    std::optional<NativeLease> acquireNative();
    int initialize(rpc::RpcDeadline const&);
    // Caller retains its NativeLease through admission and the returned use.
    // Publication uses the same mutex as initialization; no RPC holds it.
    std::pair<int, ClientQueryService*> admitReader(rpc::RpcDeadline const&);
    std::atomic<bool> initializationSettled{false};
    std::shared_ptr<RpcPolicyContext> const policy;
    ChronologTimer timer;
    ClientPortalServiceConf const portalConf;
    ClientQueryServiceConf const queryConf;
    bool const readerMode;
    std::timed_mutex metadataMutex;
    MetadataOperationState mutations;
    bool connected = false;
    std::atomic<ClientId> clientId{0};
    ClientIdentity identity;
    uint32_t euid = 0;
    std::shared_ptr<thallium::engine> engine;
    std::shared_ptr<ClientRpcProcedures> procedures;
    std::unique_ptr<RpcVisorClient> visor;
    std::unique_ptr<StorytellerClient> storyteller;
    ClientQueryService* reader = nullptr;

private:
    friend struct ClientRpcContextTestAccess;
    // Test observer is installed before first Connect. It never replaces setup
    // or ownership; it observes actual construction while the init lock is held.
    std::function<void()> beforeInitializeUnlock_;
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> facades_{0};
    std::atomic<unsigned> nativeUses_{0};
    std::timed_mutex initializeMutex_;
    ClientNativeRuntime native_;
    int initializeStatus_ = CL_SUCCESS;
};
} // namespace chronolog
#endif
