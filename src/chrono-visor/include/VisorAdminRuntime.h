#ifndef CHRONOLOG_VISOR_ADMIN_RUNTIME_H
#define CHRONOLOG_VISOR_ADMIN_RUNTIME_H
#include <RpcRuntime.h>
#include <private/RpcProviderRegistration.h>
#include <private/RpcCallState.h>
#include <array>
#include <chronolog_types.h>
#include <RpcPolicy.h>
#include <thallium.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <ServiceId.h>
#include <KeeperIdCard.h>
namespace chronolog
{
class KeeperRegistry;
enum class AdminAction;
class DataStoreAdminClient;
class AdminLookup
{
public:
    virtual ~AdminLookup() = default;
    virtual thallium::endpoint resolve(thallium::engine&, std::string const&) = 0;
};
struct AdminObservation
{
    std::atomic<unsigned> shutdownNoReplyRunReturns{0};
    std::atomic<int> shutdownExceptionKind{0};
    std::atomic<int> nativeShutdownError{0};
};
// Legacy fixtures own their existing runtime here. The native daemon binds
// weakly to its one externally retained C32; no owner/Core backlink cycle.
class VisorControlRuntime
{
    std::shared_ptr<rpc::RpcRuntime> legacy_;
    std::weak_ptr<rpc::RpcRuntime> actual_;

public:
    VisorControlRuntime(std::size_t capacity, std::shared_ptr<rpc::RpcRuntime> const& actual)
    {
        if(actual)
            actual_ = actual;
        else
        {
            legacy_ = std::make_shared<rpc::RpcRuntime>(capacity);
            actual_ = legacy_;
        }
    }
    rpc::RpcReservation reserve(rpc::RpcDeadline const& deadline)
    {
        auto runtime = actual_.lock();
        return runtime ? runtime->reserve(deadline) : rpc::RpcReservation{};
    }
    template <class Reply>
    rpc::RpcPending<Reply> submit(rpc::RpcReservation&& token, std::shared_ptr<rpc::OwnedRpcWork<Reply>> work)
    {
        return rpc::RpcRuntime::submitReserved<Reply>(std::move(token), std::move(work));
    }
    template <class Reply>
    rpc::RpcResult<Reply> invoke(rpc::RpcReservation&& token, std::shared_ptr<rpc::OwnedRpcWork<Reply>> work)
    {
        return submit<Reply>(std::move(token), std::move(work)).wait();
    }
};
struct VisorAdminRuntime
{
    explicit VisorAdminRuntime(RpcTimeoutPolicy const& policy,
                               std::size_t capacity,
                               std::shared_ptr<AdminLookup> lookup = {},
                               std::shared_ptr<rpc::RpcRuntime> actual = {});
    const RpcTimeoutPolicy policy;
    VisorControlRuntime runtime;
    const std::shared_ptr<AdminLookup> lookup;
    const std::shared_ptr<AdminObservation> observation = std::make_shared<AdminObservation>();
    std::shared_ptr<thallium::engine> engine;
    std::array<thallium::remote_procedure, 7> procedures;
    void initializeRpcDefinitions();
    void releaseNativeDescriptors(); // Only after the actual MID's native fence.
    int provision(AdminAction, uint16_t provider, rpc::RpcDeadline const&);

private:
    std::unique_ptr<rpc::RpcProviderRegistration> registrations_;
};
enum class AdminAction
{
    Start,
    StartPlayer,
    Stop,
    DestroyStory,
    DestroyChronicle,
    Shutdown,
    Available
};
struct AdminArguments
{
    AdminAction action;
    std::string chronicle, story;
    StoryId id = 0;
    uint64_t start = 0;
    std::vector<ServiceId> keepers;
};
struct AdminCompletion
{
    std::atomic<bool> done{false};
    int status = CL_ERR_OUTCOME_UNCONFIRMED;
    bool confirmed = false;
    bool dispatched = false;
};
class AdminWork final: public rpc::OwnedRpcWork<int>
{
public:
    AdminWork(std::shared_ptr<DataStoreAdminClient>,
              AdminArguments,
              std::shared_ptr<AdminCompletion>,
              std::shared_ptr<KeeperRegistry>);
    ~AdminWork() override;
    rpc::RpcResult<int> run(rpc::OperationContext&) override;

private:
    std::shared_ptr<DataStoreAdminClient> client_;
    AdminArguments arguments_;
    std::shared_ptr<AdminCompletion> completion_;
    std::shared_ptr<KeeperRegistry> registry_;
    rpc::RpcCallState native_;
    bool entered_ = false;
};
struct StoryRecording
{
    enum Phase
    {
        Starting,
        Active,
        Closing,
        Unresolved
    };
    Phase phase = Starting;
    std::string chronicle, story;
    StoryId id;
    uint64_t generation;
    std::vector<KeeperIdCard> keepers, confirmedKeepers;
    std::vector<std::shared_ptr<DataStoreAdminClient>> clients;
    std::vector<std::shared_ptr<AdminCompletion>> starts;
    ServiceId player;
};
} // namespace chronolog
#endif
