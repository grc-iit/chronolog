#ifndef CHRONOLOG_RPC_BULK_LIFETIME_H
#define CHRONOLOG_RPC_BULK_LIFETIME_H
#include <RpcDeadline.h>
#include <thallium.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <tuple>
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

// request.respond(reply), given up after timeoutMs (0: no limit). thallium
// 0.17 has no timed respond, so this encodes reply as respond does and calls
// margo_respond_timed. At the timeout Margo cancels the response and returns
// HG_TIMEOUT once Mercury is done with it.
template <class Reply>
hg_return_t respondTimed(thallium::request const& request, Reply const& reply, uint64_t timeoutMs)
{
    if(timeoutMs == 0)
    {
        request.respond(reply);
        return HG_SUCCESS;
    }
    hg_handle_t handle = request.native_handle();
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    auto args = std::make_tuple(std::cref(reply));
    std::tuple<> context;
    thallium::meta_proc_fn encode = [&args, mid, &context](hg_proc_t proc)
    { return thallium::proc_object_encode(proc, args, mid, context); };
    return margo_respond_timed(handle, &encode, static_cast<double>(timeoutMs));
}

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
    // The time a whole pull may take, and an answer to the sender (0: no
    // limit). RpcBulkOwner sets them from its policy's transfer_ms and
    // attempt_ms. A pull or an answer to a sender that died returns at its
    // limit, so it cannot keep a slot.
    uint64_t pullMs = 0, answerMs = 0;
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
            if(respondTimed(request, reply, answerMs) == HG_SUCCESS)
            {
                sent = true;
                state_->refused.fetch_add(1, std::memory_order_relaxed);
            }
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
                       std::size_t pieceBytes = kBulkPieceBytes,
                       uint64_t pullMs = 0,
                       uint64_t answerMs = 0)
        : owner_(std::move(owner))
        , engine_(engine)
        , observers_(std::move(observers))
        , pieceBytes_(pieceBytes ? pieceBytes : kBulkPieceBytes)
        , pullMs_(pullMs)
        , answerMs_(answerMs)
        , request_(request)
        , source_(source)
    {}
    bool pull()
    {
        auto const until = RpcDeadline::Clock::now() + std::chrono::milliseconds(pullMs_);
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
            // Each piece gets what is left of the pull's time.
            double timeoutMs = 0;
            if(pullMs_)
            {
                auto left = std::chrono::duration<double, std::milli>(until - RpcDeadline::Clock::now()).count();
                if(left < 1)
                {
                    timedOut = true;
                    return false;
                }
                timeoutMs = left;
            }
            auto rc = margo_bulk_itransfer_timed(engine_.get_margo_instance(),
                                                 HG_BULK_PULL,
                                                 endpoint_->get_addr(),
                                                 source_.get_bulk(),
                                                 offset,
                                                 target_->get_bulk(),
                                                 offset,
                                                 size,
                                                 timeoutMs,
                                                 &native_);
            if(rc != HG_SUCCESS)
                return false;
            if(observers_.initiated)
                observers_.initiated(native_);
            // Returns after Mercury is done with the piece, also when the
            // timeout cancelled it.
            rc = margo_wait(native_);
            native_ = nullptr;
            if(rc != HG_SUCCESS)
            {
                timedOut = rc == HG_TIMEOUT;
                return false;
            }
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
            return respondTimed(request_, reply, answerMs_) == HG_SUCCESS;
        }
        catch(...)
        {
            return false;
        }
    }
    bool initiated = false, completed = false, timedOut = false;
    std::size_t pieces = 0;
    std::vector<char> bytes;

private:
    // Native objects die before source owner. Actual owner also protects the
    // provider and explicit teardown, not just a weak raw-MID engine wrapper.
    std::shared_ptr<void> owner_;
    thallium::engine& engine_;
    BulkReceives::Observers observers_;
    std::size_t pieceBytes_;
    uint64_t pullMs_, answerMs_;
    thallium::request request_;
    thallium::bulk source_;
    std::optional<thallium::endpoint> endpoint_;
    std::optional<thallium::bulk> target_;
    margo_request native_ = nullptr;
};
} // namespace chronolog::rpc
#endif
