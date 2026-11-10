#ifndef CHRONOLOG_RPC_BULK_LIFETIME_H
#define CHRONOLOG_RPC_BULK_LIFETIME_H
#include <RpcDeadline.h>
#include <thallium.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
namespace chronolog::rpc
{
// A receiver pulls a sender's bulk in pieces of at most this many bytes, one
// piece after the other, as the sprint streams a chunk in 1 MiB frames. The
// grapher and the player read their own value from the configuration.
constexpr std::size_t kBulkPieceBytes = std::size_t{1} << 20;

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
        // Refusal replies being sent; also two at most, so a sender that
        // stopped reading cannot tie up more handler threads.
        std::atomic<unsigned> refusing{0};
        std::atomic<unsigned> refused{0};
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    // Private native fault/barrier observers, installed only before exposure.
    // Production leaves them empty; they replace no pull, registration or reply.
    struct Observers
    {
        std::function<void()> allocated;
        // Called once for each piece pulled.
        std::function<void(margo_request)> initiated;
        std::function<void()> beforeAnswer;
    } observers;
    // Set before exposure, as the observers are.
    std::size_t pieceBytes = kBulkPieceBytes;
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
    // Answers a request that found no free slot with reply, which must say
    // nothing was pulled. Without a free refusal permit either it answers
    // nothing, as before. Never throws; true if the reply was sent.
    template <class Reply>
    bool refuse(thallium::request const& request, Reply const& reply) noexcept
    {
        unsigned observed = state_->refusing.load(std::memory_order_acquire);
        do {
            if(observed >= 2)
                return false;
        } while(!state_->refusing.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel));
        bool sent = false;
        try
        {
            request.respond(reply);
            sent = true;
            state_->refused.fetch_add(1, std::memory_order_relaxed);
        }
        catch(...)
        {}
        state_->refusing.fetch_sub(1, std::memory_order_acq_rel);
        return sent;
    }
    // Requests answered by refuse().
    unsigned refused() const noexcept { return state_->refused.load(std::memory_order_acquire); }
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
                       BulkReceives::Observers observers = {},
                       std::size_t pieceBytes = kBulkPieceBytes)
        : owner_(std::move(owner))
        , engine_(engine)
        , observers_(std::move(observers))
        , pieceBytes_(pieceBytes ? pieceBytes : kBulkPieceBytes)
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
        // One piece at a time into the same target, at the same offset as in
        // the source; only the last piece completes the pull.
        for(std::size_t offset = 0; offset < bytes.size();)
        {
            std::size_t const size = std::min(pieceBytes_, bytes.size() - offset);
            auto rc = margo_bulk_itransfer(engine_.get_margo_instance(),
                                           HG_BULK_PULL,
                                           endpoint_->get_addr(),
                                           source_.get_bulk(),
                                           offset,
                                           target_->get_bulk(),
                                           offset,
                                           size,
                                           &native_);
            if(rc != HG_SUCCESS)
                return false;
            if(observers_.initiated)
                observers_.initiated(native_);
            rc = margo_wait(native_);
            native_ = nullptr;
            if(rc != HG_SUCCESS)
                return false;
            offset += size;
            ++pieces;
        }
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
    std::size_t pieces = 0;
    std::vector<char> bytes;

private:
    // Native objects die before source owner. Actual owner also protects the
    // provider and explicit teardown, not just a weak raw-MID engine wrapper.
    std::shared_ptr<void> owner_;
    thallium::engine& engine_;
    BulkReceives::Observers observers_;
    std::size_t pieceBytes_;
    thallium::request request_;
    thallium::bulk source_;
    std::optional<thallium::endpoint> endpoint_;
    std::optional<thallium::bulk> target_;
    margo_request native_ = nullptr;
};
} // namespace chronolog::rpc
#endif
