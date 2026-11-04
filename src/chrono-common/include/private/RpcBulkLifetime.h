#ifndef CHRONOLOG_RPC_BULK_LIFETIME_H
#define CHRONOLOG_RPC_BULK_LIFETIME_H
#include <RpcDeadline.h>
#include <thallium.hpp>
#include <array>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
namespace chronolog::rpc
{
// Separate from the control capacity. Payload sizes are not capped.
class BulkPermits
{
    struct State
    {
        std::atomic<unsigned> used{0};
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    class Permit
    {
        friend class BulkPermits;
        std::shared_ptr<State> state_;
        explicit Permit(std::shared_ptr<State> state)
            : state_(std::move(state))
        {}

    public:
        Permit(Permit const&) = delete;
        Permit(Permit&&) = default;
        Permit& operator=(Permit&&) = delete;
        ~Permit()
        {
            if(state_)
                state_->used.fetch_sub(1, std::memory_order_acq_rel);
        }
    };
    std::optional<Permit> acquire()
    {
        unsigned observed = state_->used.load(std::memory_order_acquire);
        while(observed < 2)
            if(state_->used.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
                return Permit(state_);
        return std::nullopt;
    }
    unsigned used() const noexcept { return state_->used.load(std::memory_order_acquire); }
};

// One per ACTUAL receiver MID, shared across every sending endpoint. Unknown
// native work stays in its same slot; there is no replacement context/queue.
class BulkReceives
{
    struct Slot
    {
        std::atomic<bool> occupied{false};
        std::shared_ptr<void> retained;
    };
    struct State
    {
        std::array<Slot, 2> slots;
        std::atomic<unsigned> rejected{0};
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    // Private native fault/barrier observers, installed only before exposure.
    // Production leaves them empty; they replace no pull, registration or reply.
    struct Observers
    {
        std::function<void()> allocated;
        std::function<void(margo_request)> initiated;
        std::function<void()> beforeAnswer;
    } observers;
    class Lease
    {
        friend class BulkReceives;
        std::shared_ptr<State> state_;
        size_t slot_;
        bool quarantined_ = false;
        Lease(std::shared_ptr<State> state, size_t slot)
            : state_(std::move(state))
            , slot_(slot)
        {}

    public:
        Lease(Lease const&) = delete;
        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_))
            , slot_(other.slot_)
            , quarantined_(other.quarantined_)
        {}
        ~Lease()
        {
            if(state_ && !quarantined_)
            {
                state_->slots[slot_].retained.reset();
                state_->slots[slot_].occupied.store(false, std::memory_order_release);
            }
        }
        void retain(std::shared_ptr<void> context) { state_->slots[slot_].retained = std::move(context); }
        void quarantine() noexcept { quarantined_ = true; }
    };
    std::optional<Lease> acquire()
    {
        for(size_t i = 0; i < 2; ++i)
        {
            bool expected = false;
            if(state_->slots[i].occupied.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return Lease(state_, i);
        }
        state_->rejected.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    unsigned rejected() const noexcept { return state_->rejected.load(std::memory_order_acquire); }
    unsigned used() const noexcept
    {
        unsigned result = 0;
        for(auto const& slot: state_->slots) result += slot.occupied.load(std::memory_order_acquire);
        return result;
    }
};

class BulkReceiveContext
{
public:
    BulkReceiveContext(std::shared_ptr<void> owner,
                       thallium::engine& engine,
                       thallium::request const& request,
                       thallium::bulk const& source,
                       BulkReceives::Observers observers = {})
        : owner_(std::move(owner))
        , engine_(engine)
        , observers_(std::move(observers))
        , request_(request)
        , source_(source)
    {}
    bool pull()
    {
        bytes.resize(source_.size());
        endpoint_.emplace(request_.get_endpoint());
        target_.emplace(engine_.expose({{bytes.data(), bytes.size()}}, thallium::bulk_mode::write_only));
        if(observers_.allocated)
            observers_.allocated();
        // Any error after initiation is conservatively unknown. Keep the
        // original descriptor/endpoint and target until actual completion.
        initiated = true;
        auto rc = margo_bulk_itransfer(engine_.get_margo_instance(),
                                       HG_BULK_PULL,
                                       endpoint_->get_addr(),
                                       source_.get_bulk(),
                                       0,
                                       target_->get_bulk(),
                                       0,
                                       bytes.size(),
                                       &native_);
        if(rc != HG_SUCCESS)
            return false;
        if(observers_.initiated)
            observers_.initiated(native_);
        rc = margo_wait(native_);
        native_ = nullptr;
        if(rc != HG_SUCCESS)
            return false;
        completed = true;
        return true;
    }
    template <class Reply>
    bool answer(Reply const& reply) noexcept
    {
        try
        {
            if(observers_.beforeAnswer)
                observers_.beforeAnswer();
            request_.respond(reply);
            return true;
        }
        catch(...)
        {
            return false;
        }
    }
    bool initiated = false, completed = false;
    std::vector<char> bytes;

private:
    // Native objects die before source owner. Actual owner also protects the
    // provider and explicit teardown, not just a weak raw-MID engine wrapper.
    std::shared_ptr<void> owner_;
    thallium::engine& engine_;
    BulkReceives::Observers observers_;
    thallium::request request_;
    thallium::bulk source_;
    std::optional<thallium::endpoint> endpoint_;
    std::optional<thallium::bulk> target_;
    margo_request native_ = nullptr;
};
} // namespace chronolog::rpc
#endif
