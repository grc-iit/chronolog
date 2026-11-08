#ifndef CHRONOLOG_REGISTRY_SESSION_H
#define CHRONOLOG_REGISTRY_SESSION_H
#include <private/RpcBulkOwner.h>
#include <private/RpcCallState.h>
#include <array>
#include <csignal>
#include <thread>
namespace chronolog::rpc
{
// One descriptor/cache on the actual admin MID. It never owns that MID or its
// runtime; each admitted call retains both, its callable, and copied arguments.
class RegistrySession: public std::enable_shared_from_this<RegistrySession>
{
    friend struct RegistrySessionTestAccess; // Private test observation, no runtime hook or state.
public:
    enum Action
    {
        Register,
        Stats,
        Unregister
    };
    struct Completion
    {
        std::atomic<bool> done{false};
        bool known = false, dispatched = false;
        int status = CL_ERR_OUTCOME_UNCONFIRMED;
    };
    RegistrySession(std::shared_ptr<RpcBulkOwner> const& owner,
                    std::string kind,
                    std::string address,
                    uint16_t provider)
        : owner_(owner)
        , address_(std::move(address))
        , provider_(provider)
    {
        names_ = {"register_" + kind,
                  kind == "keeper" ? "handle_stats_msg" : "handle_" + kind + "_stats_msg",
                  "unregister_" + kind};
    }
    static std::shared_ptr<RegistrySession> Create(std::shared_ptr<RpcBulkOwner> const& owner,
                                                   std::string kind,
                                                   std::string address,
                                                   uint16_t provider,
                                                   RpcDeadline const& deadline)
    {
        auto runtime = owner ? owner->runtime.lock() : nullptr;
        if(!runtime || !owner->engine || owner->registrySession)
            return {};
        auto reservation = runtime->reserve(deadline);
        if(!reservation)
            return {};
        auto session = std::make_shared<RegistrySession>(owner, std::move(kind), std::move(address), provider);
        owner->registrySession = session; // Root precedes the first native definition.
        class Initialize final: public OwnedRpcWork<int>
        {
            std::shared_ptr<RegistrySession> session_;
            std::shared_ptr<RpcBulkOwner> owner_;

        public:
            Initialize(std::shared_ptr<RegistrySession> session, std::shared_ptr<RpcBulkOwner> owner)
                : session_(std::move(session))
                , owner_(std::move(owner))
            {}
            RpcResult<int> run(OperationContext& operation) override
            {
                if(operation.deadline().expired())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, CL_ERR_DEADLINE_EXCEEDED};
                session_->initialize();
                return {CL_SUCCESS, false, CL_SUCCESS};
            }
        };
        auto result = runtime->invoke<int>(std::move(reservation), std::make_shared<Initialize>(session, owner));
        return result.reply && *result.reply == CL_SUCCESS ? session : nullptr;
    }
    // Called once on an admitted native startup worker, before use/exposure.
    void initialize()
    {
        if(attempted_)
            throw std::logic_error("Registry procedures cannot be initialized twice");
        attempted_ = true;
        auto owner = owner_.lock();
        if(!owner || !owner->engine)
            throw std::logic_error("Registry session needs its actual admin MID");
        for(std::size_t i = 0; i < calls_.size(); ++i) calls_[i] = owner->engine->define(names_[i]);
        calls_[Stats].disable_response();
        initialized_.store(true, std::memory_order_release);
    }
    template <class Message>
    int send(Action action,
             Message const& message,
             RpcDeadline const& supplied,
             bool wait = true,
             volatile sig_atomic_t const* interrupt = nullptr)
    {
        auto entered = RpcDeadline::Clock::now();
        auto owner = owner_.lock();
        if(!owner || !initialized_.load(std::memory_order_acquire))
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        auto limit = RpcDeadline::fromStart(entered, std::chrono::milliseconds(owner->policy.attempt_ms));
        auto deadline = supplied.expiresAt() < limit.expiresAt() ? supplied : limit;
        auto runtime = owner->runtime.lock();
        if(!runtime)
            return CL_ERR_SHUTDOWN_INCOMPLETE;
        std::shared_ptr<Completion> completion;
        {
            std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
            if((wait ? !lock.try_lock_until(deadline.expiresAt()) : !lock.try_lock()) || deadline.expired())
                return CL_ERR_DEADLINE_EXCEEDED;
            auto& current = pending_[action];
            if(current && current->done.load(std::memory_order_acquire) && current->known &&
               (action == Stats || current->status != CL_SUCCESS))
                current.reset();
            if(!current)
            {
                auto reservation = runtime->reserve(deadline);
                if(!reservation)
                    return reservation.status();
                auto prepared = std::make_shared<Completion>();
                auto work = std::make_shared<Call<Message>>(shared_from_this(), owner, action, message, prepared);
                runtime->submit<int>(std::move(reservation), std::move(work)).releaseToRuntime();
                current = std::move(prepared);
            }
            completion = current;
        }
        if(!wait)
            return CL_SUCCESS; // Stats admission only; local send is no remote ACK.
        while(!completion->done.load(std::memory_order_acquire) && !deadline.expired() && (!interrupt || !*interrupt))
            std::this_thread::sleep_for(
                    std::min(deadline.remaining(),
                             std::chrono::duration_cast<RpcDeadline::Clock::duration>(std::chrono::milliseconds(1))));
        if(!completion->done.load(std::memory_order_acquire))
            return CL_ERR_DEADLINE_EXCEEDED;
        return completion->status;
    }

private:
    template <class Message>
    class Call final: public OwnedRpcWork<int>
    {
        std::shared_ptr<RegistrySession> session_;
        std::shared_ptr<RpcBulkOwner> owner_;
        Action action_;
        Message message_;
        std::shared_ptr<Completion> completion_;
        RpcCallState native_;
        bool entered_ = false;

    public:
        Call(std::shared_ptr<RegistrySession> session,
             std::shared_ptr<RpcBulkOwner> owner,
             Action action,
             Message const& message,
             std::shared_ptr<Completion> completion)
            : session_(std::move(session))
            , owner_(std::move(owner))
            , action_(action)
            , message_(message)
            , completion_(std::move(completion))
        {}
        ~Call() override
        {
            if(!entered_)
            {
                completion_->status = CL_ERR_DEADLINE_EXCEEDED;
                completion_->known = true;
                completion_->done.store(true, std::memory_order_release);
            }
        }
        RpcResult<int> run(OperationContext& operation) override
        {
            entered_ = true;
            bool dispatched = false;
            int status =
                    owner_->provision(session_->names_[action_].c_str(), session_->provider_, operation.deadline());
            if(status == CL_SUCCESS)
            {
                try
                {
                    native_.bind(*owner_->engine, session_->calls_[action_], session_->address_, session_->provider_);
                    if(!operation.tryBeginDispatch())
                        status = CL_ERR_DEADLINE_EXCEEDED;
                    else
                    {
                        dispatched = true;
                        if(action_ == Stats)
                        {
                            native_.callable()(message_);
                            status = CL_SUCCESS;
                        }
                        else
                            status = native_.callable()(message_);
                        native_.clearKnown();
                        completion_->known = true;
                    }
                }
                catch(...)
                {
                    status = dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN;
                }
            }
            if(!dispatched)
                completion_->known = true;
            completion_->status = status;
            completion_->dispatched = dispatched;
            completion_->done.store(true, std::memory_order_release);
            return {status, dispatched, completion_->known ? std::optional<int>(status) : std::nullopt};
        }
    };
    std::weak_ptr<RpcBulkOwner> owner_;
    std::string address_;
    uint16_t provider_;
    std::array<std::string, 3> names_;
    std::array<thallium::remote_procedure, 3> calls_;
    std::array<std::shared_ptr<Completion>, 3> pending_;
    std::timed_mutex mutex_;
    bool attempted_ = false;
    std::atomic<bool> initialized_{false};
};
} // namespace chronolog::rpc
#endif
