#include "ChronologClientImpl.h"
#include "ClientQueryService.h"
#include "StorytellerClient.h"
#include "rpcVisorClient.h"
#include <stdexcept>
namespace chronolog
{
std::mutex ChronologClientImpl::factoryMutex_;
std::weak_ptr<ClientRpcContext> ChronologClientImpl::instance_;
ChronologClientImpl::ChronologClientImpl(std::shared_ptr<ClientRpcContext> context,
                                         std::shared_ptr<rpc::RpcRuntime> runtime,
                                         bool admitted)
    : context_(std::move(context))
    , runtime_(std::move(runtime))
    , facadeAdmitted_(admitted)
{}
std::shared_ptr<ChronologClientImpl> ChronologClientImpl::GetClientImplInstance(ClientPortalServiceConf const& portal,
                                                                                ClientMode const& mode,
                                                                                ClientQueryServiceConf const& query,
                                                                                ChronologTimer::RawSource source)
{
    std::lock_guard<std::mutex> lock(factoryMutex_);
    auto context = instance_.lock();
    if(context && !context->cleanupComplete.load(std::memory_order_acquire))
    {
        if(source)
            throw std::invalid_argument("Cannot replace the clock source of an existing client context");
        // Lock the runtime BEFORE retaining a facade. The last old facade can
        // otherwise drop that runtime between the count CAS and weak lock.
        auto runtime = context->runtime.lock();
        bool admitted = runtime && context->retainFacade();
        if(!admitted)
            runtime.reset();
        try
        {
            auto* facade = new ChronologClientImpl(context, runtime, admitted);
            admitted = false; // shared_ptr construction deletes facade if allocation fails
            return std::shared_ptr<ChronologClientImpl>(facade);
        }
        catch(...)
        {
            if(admitted)
                context->releaseFacade();
            throw;
        }
    }
    context = std::make_shared<ClientRpcContext>(portal, mode == READER_MODE, query, std::move(source));
    auto runtime = std::make_shared<rpc::RpcRuntime>();
    context->runtime = runtime;
    runtime->retainOwner(context, [context] { context->cleanup(); });
    context->retainFacade();
    instance_ = context;
    ChronologClientImpl* facade;
    try
    {
        facade = new ChronologClientImpl(context, runtime, true);
    }
    catch(...)
    {
        context->releaseFacade();
        throw;
    }
    return std::shared_ptr<ChronologClientImpl>(facade);
}
ChronologClientImpl::~ChronologClientImpl()
{
    if(facadeAdmitted_)
        context_->releaseFacade();
    // Context native fields are rooted by runtime state; this drops only a
    // facade/runtime handle. Worker cleanup owns actual native finalization.
}
int ChronologClientImpl::SetRpcTimeoutPolicy(RpcTimeoutPolicy const& policy)
{
    if(validateRpcTimeoutPolicy(policy) != CL_SUCCESS)
        return CL_ERR_INVALID_ARG;
    return runtime_ && !context_->closing() ? context_->policy->setPolicy(policy) : CL_ERR_RPC_CAPACITY;
}
class ChronologClientImpl::Work final: public rpc::OwnedRpcWork<Reply>
{
public:
    Work(std::shared_ptr<ClientRpcContext> owner,
         RpcPolicyContext::Snapshot snapshot,
         Action action,
         std::string chronicle,
         std::string story)
        : owner_(std::move(owner))
        , snapshot_(std::move(snapshot))
        , action_(action)
        , chronicle_(std::move(chronicle))
        , story_(std::move(story))
    {}
    rpc::RpcResult<Reply> run(rpc::OperationContext& operation) override
    {
        if(action_ == Action::Connect)
        {
            int status = owner_->initialize(operation.deadline());
            if(status != CL_SUCCESS)
                return {status, false, std::nullopt};
        }
        std::unique_lock<std::timed_mutex> lock(owner_->metadataMutex, std::defer_lock);
        if(!lock.try_lock_until(operation.deadline().expiresAt()))
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        bool readOnly = action_ == Action::ShowChronicles || action_ == Action::ShowStories;
        std::optional<MetadataOperationState::Ticket> ticket;
        if(!readOnly)
        {
            ticket = owner_->mutations.begin(chronicle_, story_);
            if(!ticket)
                return local(CL_ERR_OUTCOME_UNCONFIRMED);
        }
        auto reject = [&](int status, StoryHandle* handle = nullptr)
        {
            if(ticket)
                owner_->mutations.finish(*ticket, true);
            return local(status, handle);
        };
        if(action_ == Action::Connect && owner_->connected)
            return reject(CL_SUCCESS);
        if(action_ == Action::Disconnect && !owner_->connected)
            return reject(CL_SUCCESS);
        if(action_ != Action::Connect && !owner_->connected)
            return reject(CL_ERR_NO_CONNECTION);
        if(action_ == Action::AcquireStory && owner_->storyteller)
            if(auto handle = owner_->storyteller->findStoryHandle(chronicle_, story_))
                return reject(CL_SUCCESS, handle);
        if(action_ == Action::ReleaseStory &&
           (!owner_->storyteller || !owner_->storyteller->findStoryHandle(chronicle_, story_)))
            return reject(CL_ERR_NOT_ACQUIRED);
        lock.unlock();
        auto result = execute(operation);
        // This is worker-only state publication. The public result is private
        // staging; no returned caller frame/output is ever retained here.
        lock.lock();
        if(ticket && !owner_->mutations.matches(*ticket))
            return local(CL_ERR_OUTCOME_UNCONFIRMED);
        if(result.reply)
        {
            auto& reply = *result.reply;
            bool uncertain = reply.status == CL_ERR_OUTCOME_UNCONFIRMED || reply.status == CL_ERR_DEADLINE_EXCEEDED ||
                             reply.status == CL_ERR_RPC_CAPACITY;
            if(!readOnly && uncertain)
                reply.status = CL_ERR_OUTCOME_UNCONFIRMED;
            if(reply.status == CL_SUCCESS)
            {
                if(action_ == Action::Connect)
                {
                    owner_->clientId.store(reply.id, std::memory_order_release);
                    if(!owner_->storyteller)
                        owner_->storyteller = std::make_unique<StorytellerClient>(owner_->timer,
                                                                                  *owner_->engine,
                                                                                  reply.id,
                                                                                  owner_->policy,
                                                                                  0,
                                                                                  owner_,
                                                                                  owner_->procedures);
                    owner_->connected = true;
                }
                else if(action_ == Action::AcquireStory)
                {
                    auto const& acquisition = *reply.acquisition;
                    reply.handle = owner_->storyteller->initializeStoryHandle(chronicle_,
                                                                              story_,
                                                                              acquisition.getStoryId(),
                                                                              acquisition.getKeepers(),
                                                                              acquisition.getPlayer());
                    if(!reply.handle)
                    {
                        reply.status = CL_ERR_OUTCOME_UNCONFIRMED;
                        uncertain = true;
                    }
                    if(reply.handle && owner_->reader && acquisition.getPlayer().is_valid())
                        owner_->reader->addStoryReader(chronicle_,
                                                       story_,
                                                       acquisition.getStoryId(),
                                                       acquisition.getPlayer());
                    reply.acquisition.reset();
                }
                else if(action_ == Action::ReleaseStory)
                {
                    owner_->storyteller->removeAcquiredStoryHandle(chronicle_, story_);
                    if(owner_->reader)
                        owner_->reader->removeStoryReader(chronicle_, story_);
                }
                else if(action_ == Action::Disconnect)
                {
                    owner_->connected = false;
                    owner_->clientId.store(0, std::memory_order_release);
                    owner_->storyteller->clearAcquiredStoryHandles();
                }
            }
            if(ticket)
                owner_->mutations.finish(*ticket, !uncertain);
        }
        else if(ticket)
            owner_->mutations.finish(*ticket, !result.may_have_dispatched);
        if(readOnly && !result.reply && result.may_have_dispatched)
            result.status = CL_ERR_DEADLINE_EXCEEDED;
        return result;
    }

private:
    rpc::RpcResult<Reply> local(int status, StoryHandle* handle = nullptr)
    {
        return {CL_SUCCESS, false, Reply{status, handle, {}}};
    }
    rpc::RpcResult<Reply> execute(rpc::OperationContext& operation)
    {
        auto& visor = *owner_->visor;
        auto attempt = snapshot_.policy().attempt_ms;
        auto id = owner_->clientId.load(std::memory_order_acquire);
        if(action_ == Action::Connect)
        {
            auto result = visor.call<ConnectResponseMsg>(ClientRpcProcedures::Connect,
                                                         operation,
                                                         attempt,
                                                         native_,
                                                         owner_->euid,
                                                         owner_->identity.pack(),
                                                         CLIENT_PROTOCOL_VERSION);
            if(!result.reply)
                return {result.status, result.may_have_dispatched, std::nullopt};
            return {CL_SUCCESS, true, Reply{result.reply->getErrorCode(), nullptr, {}, result.reply->getClientId()}};
        }
        if(action_ == Action::AcquireStory)
        {
            auto result = visor.call<AcquireStoryResponseMsg>(ClientRpcProcedures::AcquireStory,
                                                              operation,
                                                              attempt,
                                                              native_,
                                                              id,
                                                              chronicle_,
                                                              story_);
            if(!result.reply)
                return {result.status, result.may_have_dispatched, std::nullopt};
            auto status = result.reply->getErrorCode();
            if(status == CL_ERR_ACQUIRED || (status == CL_SUCCESS && result.reply->getKeepers().empty()))
                status = CL_ERR_OUTCOME_UNCONFIRMED;
            Reply reply{status, nullptr, {}};
            if(status == CL_SUCCESS)
                reply.acquisition = std::make_shared<AcquireStoryResponseMsg>(*result.reply);
            return {CL_SUCCESS, true, std::move(reply)};
        }
        if(action_ == Action::ShowChronicles || action_ == Action::ShowStories)
        {
            using Names = std::pair<int, std::vector<std::string>>;
            auto result =
                    action_ == Action::ShowChronicles
                            ? visor.call<Names>(ClientRpcProcedures::ShowChronicles, operation, attempt, native_, id)
                            : visor.call<Names>(ClientRpcProcedures::ShowStories,
                                                operation,
                                                attempt,
                                                native_,
                                                id,
                                                chronicle_);
            if(!result.reply)
                return {result.status, result.may_have_dispatched, std::nullopt};
            return {CL_SUCCESS, true, Reply{result.reply->first, nullptr, std::move(result.reply->second)}};
        }
        rpc::RpcResult<int> result{CL_ERR_UNKNOWN, false, std::nullopt};
        switch(action_)
        {
            case Action::Disconnect:
                result = visor.call<int>(ClientRpcProcedures::Disconnect, operation, attempt, native_, id);
                break;
            case Action::CreateChronicle:
                result = visor.call<int>(ClientRpcProcedures::CreateChronicle,
                                         operation,
                                         attempt,
                                         native_,
                                         id,
                                         chronicle_);
                break;
            case Action::DestroyChronicle:
                result = visor.call<int>(ClientRpcProcedures::DestroyChronicle,
                                         operation,
                                         attempt,
                                         native_,
                                         id,
                                         chronicle_);
                break;
            case Action::ReleaseStory:
                result = visor.call<int>(ClientRpcProcedures::ReleaseStory,
                                         operation,
                                         attempt,
                                         native_,
                                         id,
                                         chronicle_,
                                         story_);
                break;
            case Action::DestroyStory:
                result = visor.call<int>(ClientRpcProcedures::DestroyStory,
                                         operation,
                                         attempt,
                                         native_,
                                         id,
                                         chronicle_,
                                         story_);
                break;
            default:
                break;
        }
        if(!result.reply)
            return {result.status, result.may_have_dispatched, std::nullopt};
        return {CL_SUCCESS, true, Reply{*result.reply, nullptr, {}}};
    }
    std::shared_ptr<ClientRpcContext> owner_;
    RpcPolicyContext::Snapshot snapshot_;
    Action action_;
    std::string chronicle_, story_;
    rpc::RpcCallState native_;
};
ChronologClientImpl::Reply ChronologClientImpl::invoke(Action action,
                                                       std::string const& chronicle,
                                                       std::string const& story,
                                                       Clock::time_point started)
{
    if(action == Action::Connect)
        context_->policy->freeze();
    if(!runtime_ || context_->closing())
        return {CL_ERR_RPC_CAPACITY, nullptr, {}};
    auto snapshot = context_->policy->tryAcquire();
    if(!snapshot)
        return {CL_ERR_RPC_CAPACITY, nullptr, {}};
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(snapshot->policy().control_ms));
    if(action != Action::ShowChronicles && action != Action::ShowStories)
    {
        // Preserve known uncertainty even while completed prior jobs still
        // occupy their retirement slots. Worker begin() rechecks after reserve.
        std::unique_lock<std::timed_mutex> lock(context_->metadataMutex, std::defer_lock);
        if(!lock.try_lock_until(deadline.expiresAt()))
            return {CL_ERR_DEADLINE_EXCEEDED, nullptr, {}};
        if(context_->mutations.conflicts(chronicle, story))
            return {CL_ERR_OUTCOME_UNCONFIRMED, nullptr, {}};
    }
    auto reservation = runtime_->reserve(deadline);
    if(!reservation)
        return {reservation.status(), nullptr, {}};
    auto result =
            runtime_->invoke<Reply>(std::move(reservation),
                                    std::make_shared<Work>(context_, std::move(*snapshot), action, chronicle, story));
    if(result.reply)
        return std::move(*result.reply);
    bool readOnly = action == Action::ShowChronicles || action == Action::ShowStories;
    return {readOnly && result.may_have_dispatched ? CL_ERR_DEADLINE_EXCEEDED : result.status, nullptr, {}};
}
int ChronologClientImpl::Connect(Clock::time_point start) { return invoke(Action::Connect, "", "", start).status; }
int ChronologClientImpl::Disconnect(Clock::time_point start)
{
    return invoke(Action::Disconnect, "", "", start).status;
}
int ChronologClientImpl::CreateChronicle(std::string const& c, Clock::time_point start)
{
    return c.empty() ? CL_ERR_INVALID_ARG : invoke(Action::CreateChronicle, c, "", start).status;
}
int ChronologClientImpl::DestroyChronicle(std::string const& c, Clock::time_point start)
{
    return c.empty() ? CL_ERR_INVALID_ARG : invoke(Action::DestroyChronicle, c, "", start).status;
}
std::pair<int, StoryHandle*>
ChronologClientImpl::AcquireStory(std::string const& c, std::string const& s, Clock::time_point start)
{
    if(c.empty() || s.empty())
        return {CL_ERR_INVALID_ARG, nullptr};
    auto result = invoke(Action::AcquireStory, c, s, start);
    return {result.status, result.handle};
}
int ChronologClientImpl::ReleaseStory(std::string const& c, std::string const& s, Clock::time_point start)
{
    return c.empty() || s.empty() ? CL_ERR_INVALID_ARG : invoke(Action::ReleaseStory, c, s, start).status;
}
int ChronologClientImpl::DestroyStory(std::string const& c, std::string const& s, Clock::time_point start)
{
    return c.empty() || s.empty() ? CL_ERR_INVALID_ARG : invoke(Action::DestroyStory, c, s, start).status;
}
std::pair<int, std::vector<std::string>> ChronologClientImpl::ShowChronicles(Clock::time_point start)
{
    auto result = invoke(Action::ShowChronicles, "", "", start);
    return {result.status, std::move(result.names)};
}
std::pair<int, std::vector<std::string>> ChronologClientImpl::ShowStories(std::string const& c, Clock::time_point start)
{
    if(c.empty())
        return {CL_ERR_INVALID_ARG, {}};
    auto result = invoke(Action::ShowStories, c, "", start);
    return {result.status, std::move(result.names)};
}
int ChronologClientImpl::replay_story(ChronicleName const& c,
                                      StoryName const& s,
                                      uint64_t begin,
                                      uint64_t end,
                                      std::vector<Event>& events,
                                      Clock::time_point started)
{
    auto lease = context_->acquireNative();
    if(!lease)
        return CL_ERR_RPC_CAPACITY;
    if(!context_->readerMode)
        return CL_ERR_NOT_READER_MODE;
    auto snapshot = context_->policy->tryAcquire();
    if(!snapshot)
        return CL_ERR_RPC_CAPACITY;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(snapshot->policy().replay_ms));
    auto admission = context_->admitReader(deadline);
    if(admission.first != CL_SUCCESS)
        return admission.first;
    // B09 must pass this original deadline through the legacy replay RPC path.
    return admission.second->replay_story(c, s, begin, end, events);
}
int ChronologClientImpl::replay_story(ChronicleName const& c,
                                      StoryName const& s,
                                      uint64_t begin,
                                      uint64_t end,
                                      Client::EventCallback callback,
                                      Clock::time_point started)
{
    auto lease = context_->acquireNative();
    if(!lease)
        return CL_ERR_RPC_CAPACITY;
    if(!context_->readerMode)
        return CL_ERR_NOT_READER_MODE;
    if(!callback)
        return CL_ERR_INVALID_ARG;
    auto snapshot = context_->policy->tryAcquire();
    if(!snapshot)
        return CL_ERR_RPC_CAPACITY;
    auto deadline = rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(snapshot->policy().replay_ms));
    auto admission = context_->admitReader(deadline);
    if(admission.first != CL_SUCCESS)
        return admission.first;
    // B09 must pass this original deadline through the legacy replay RPC path.
    return admission.second->replay_story(c, s, begin, end, std::move(callback));
}
} // namespace chronolog
