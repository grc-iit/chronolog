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
struct VisorAdminRuntime
{
    explicit VisorAdminRuntime(RpcTimeoutPolicy const& policy,
                               std::size_t capacity,
                               std::shared_ptr<AdminLookup> lookup = {});
    const RpcTimeoutPolicy policy;
    rpc::RpcRuntime runtime;
    const std::shared_ptr<AdminLookup> lookup;
    const std::shared_ptr<AdminObservation> observation = std::make_shared<AdminObservation>();
    std::shared_ptr<thallium::engine> engine;
    std::array<thallium::remote_procedure, 7> procedures;
    void initializeRpcDefinitions();
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
};
class AdminWork final: public rpc::OwnedRpcWork<int>
{
public:
    AdminWork(std::shared_ptr<DataStoreAdminClient>,
              AdminArguments,
              std::shared_ptr<AdminCompletion>,
              std::shared_ptr<KeeperRegistry>);
    rpc::RpcResult<int> run(rpc::OperationContext&) override;

private:
    std::shared_ptr<DataStoreAdminClient> client_;
    AdminArguments arguments_;
    std::shared_ptr<AdminCompletion> completion_;
    std::shared_ptr<KeeperRegistry> registry_;
    rpc::RpcCallState native_;
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
