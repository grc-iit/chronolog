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
#include <cstddef>
#include <functional>
#include <map>
#include <functional>
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
    // Called once on the worker after done is set, if installed before the
    // work is submitted.
    std::function<void(AdminCompletion const&)> finished;
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
    RecordingGroupId group = 0;
    std::vector<KeeperIdCard> keepers, confirmedKeepers;
    // clients[i] was sent starts[i]. The first originalStarts come from the
    // first acquire: the grapher, the keepers, then the player. Those after
    // went to keepers that registered while the story was active.
    std::vector<std::shared_ptr<DataStoreAdminClient>> clients;
    std::vector<std::shared_ptr<AdminCompletion>> starts;
    std::size_t originalStarts = 0;
    ServiceId player;
};

// The recordings a keeper of `group` must start when it registers: the active
// stories of that group. A keeper that registers again after a restart has no
// pipeline for the stories clients still hold, and an acquire sends
// start_story_recording only for a story's first holder. A story still
// starting, closing or unresolved is left out.
inline std::vector<std::shared_ptr<StoryRecording>>
storiesToStartOnRegistration(std::map<StoryId, std::shared_ptr<StoryRecording>> const& active, RecordingGroupId group)
{
    std::vector<std::shared_ptr<StoryRecording>> stories;
    for(auto const& [id, recording]: active)
        if(recording && recording->phase == StoryRecording::Active && recording->group == group)
            stories.push_back(recording);
    return stories;
}

// Which of a story's starts its stop goes to, by index into its clients. Every
// start must have settled first, so a stop reaches each keeper after the
// start. A start from the first acquire must have a confirmed answer, and gets
// a stop only if it succeeded; one with no confirmed answer stays unresolved.
// A start sent to a keeper that registered later gets a stop whatever its
// answer was: a failed call may still have run on the keeper, and a keeper
// that never had the story ignores the stop.
struct StoryStopTargets
{
    enum State
    {
        Ready,
        Pending,    // a start has not settled yet
        Unconfirmed // a start from the first acquire has no confirmed answer
    };
    State state = Ready;
    std::vector<std::size_t> targets;
};
inline StoryStopTargets storyStopTargets(StoryRecording const& recording)
{
    StoryStopTargets result;
    for(std::size_t i = 0; i < recording.starts.size(); ++i)
    {
        auto const& start = *recording.starts[i];
        if(!start.done.load(std::memory_order_acquire))
            return {StoryStopTargets::Pending, {}};
        if(i >= recording.originalStarts)
            result.targets.push_back(i);
        else if(!start.confirmed)
            return {StoryStopTargets::Unconfirmed, {}};
        else if(start.status == CL_SUCCESS)
            result.targets.push_back(i);
    }
    return result;
}
} // namespace chronolog
#endif
