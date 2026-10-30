#ifndef CHRONOLOG_RPC_POLICY_CONTEXT_H
#define CHRONOLOG_RPC_POLICY_CONTEXT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <RpcPolicy.h>

namespace chronolog
{
// Policy ownership for one existing client-implementation context. Downstream
// operation entry points retain a Snapshot throughout owned asynchronous work.
// This class alone does not integrate the existing RPC or StoryHandle paths.
class RpcPolicyContext
{
    friend struct RpcPolicyContextTestAccess;
    struct State
    {
        explicit State(RpcTimeoutPolicy const& initial)
            : policy(initial)
        {}
        std::mutex mutex;
        RpcTimeoutPolicy policy;
        std::atomic<size_t> active{0};
        std::atomic<bool> frozen{false};
    };

public:
    class Snapshot
    {
    public:
        Snapshot(Snapshot const&) = delete;
        Snapshot& operator=(Snapshot const&) = delete;
        Snapshot(Snapshot&& other) noexcept
            : state_(std::move(other.state_))
            , policy_(other.policy_)
        {}
        Snapshot& operator=(Snapshot&&) = delete;
        ~Snapshot()
        {
            if(state_)
                state_->active.fetch_sub(1);
        }
        RpcTimeoutPolicy const& policy() const noexcept { return policy_; }

    private:
        friend class RpcPolicyContext;
        explicit Snapshot(std::shared_ptr<State> state)
            : state_(std::move(state))
            , policy_(state_->policy)
        {}
        std::shared_ptr<State> state_;
        RpcTimeoutPolicy policy_;
    };

    explicit RpcPolicyContext(RpcTimeoutPolicy const& initial)
    {
        if(validateRpcTimeoutPolicy(initial) != CL_SUCCESS)
            throw std::invalid_argument("Invalid RpcTimeoutPolicy");
        state_ = std::make_shared<State>(initial);
    }
    RpcPolicyContext(RpcPolicyContext const&) = delete;
    RpcPolicyContext& operator=(RpcPolicyContext const&) = delete;

    int setPolicy(RpcTimeoutPolicy const& policy)
    {
        if(validateRpcTimeoutPolicy(policy) != CL_SUCCESS)
            return CL_ERR_INVALID_ARG;
        std::unique_lock<std::mutex> lock(state_->mutex, std::try_to_lock);
        if(!lock.owns_lock() || state_->frozen || state_->active.load() != 0)
            return CL_ERR_RPC_CAPACITY;
        state_->policy = policy;
        return CL_SUCCESS;
    }

    std::optional<Snapshot> tryAcquire()
    {
        std::unique_lock<std::mutex> lock(state_->mutex, std::try_to_lock);
        if(!lock.owns_lock())
            return std::nullopt;
        state_->active.fetch_add(1);
        return Snapshot(state_);
    }

    // Atomic latch is permanent even when policy admission is contended.
    void freeze() noexcept { state_->frozen.store(true, std::memory_order_release); }

private:
    std::shared_ptr<State> state_;
};
} // namespace chronolog

#endif
