#ifndef KEEPER_HOT_FETCH_CLIENT_H
#define KEEPER_HOT_FETCH_CLIENT_H
#include <PlaybackRpcOwner.h>
#include <RpcFanout.h>
#include <HotRangeResponse.h>
#include <ServiceId.h>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/tuple.hpp>
namespace chronolog
{
// Cached route descriptor; native lookup/dispatch belongs to a charged child.
class KeeperHotFetchClient
{
public:
    static constexpr std::chrono::milliseconds kDefaultFetchDeadline{5000};
    static KeeperHotFetchClient* CreateKeeperHotFetchClient(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                                            ServiceId const& route,
                                                            std::chrono::milliseconds cap = kDefaultFetchDeadline)
    {
        return new KeeperHotFetchClient(owner, route, cap);
    }
    HotRangeResponse fetchRange(StoryId story, uint64_t start, uint64_t end, uint64_t max)
    {
        auto deadline = rpc::RpcDeadline::after(cap_);
        auto owner = owner_.lock();
        auto runtime = owner ? owner->runtime.lock() : nullptr;
        if(!runtime || owner->closing())
            return unansweredResponse();
        auto reservation = runtime->reserve(deadline);
        if(!reservation)
            return unansweredResponse();
        auto pending = fetchRangeAsync(std::move(reservation), story, start, end, max);
        return waitForRange(pending);
    }
    rpc::RpcPending<HotRangeResponse> fetchRangeAsync(rpc::RpcReservation reservation,
                                                      StoryId story,
                                                      uint64_t start,
                                                      uint64_t end,
                                                      uint64_t max,
                                                      std::shared_ptr<rpc::FanoutIssueBarrier> barrier = {})
    {
        auto owner = owner_.lock();
        auto runtime = owner ? owner->runtime.lock() : nullptr;
        if(!runtime || owner->closing())
            return {};
        return runtime->submit<HotRangeResponse>(
                std::move(reservation),
                std::make_shared<Fetch>(owner, route_, cap_, story, start, end, max, std::move(barrier)));
    }
    HotRangeResponse waitForRange(rpc::RpcPending<HotRangeResponse>& pending)
    {
        auto result = pending.wait();
        return result.reply ? std::move(*result.reply) : unansweredResponse();
    }
    ServiceId const& getKeeperServiceId() const { return route_; }
    static HotRangeResponse unansweredResponse()
    {
        HotRangeResponse value;
        value.answered = false;
        return value;
    }

private:
    friend class PlaybackService;
    friend struct KeeperHotFetchClientTestAccess;
    static rpc::RpcDeadline fanoutDeadline(rpc::RpcDeadline const& replay,
                                           rpc::RpcDeadline::Clock::time_point phaseStarted,
                                           std::chrono::milliseconds cap)
    {
        // One constituent phase includes reservation, lookup, issue and collect.
        // It may shorten the replay's remaining budget, never extend it.
        auto phase = rpc::RpcDeadline::fromStart(phaseStarted, cap);
        return phase.expiresAt() < replay.expiresAt() ? phase : replay;
    }
    class Fetch final: public rpc::OwnedRpcWork<HotRangeResponse>
    {
    public:
        Fetch(std::shared_ptr<PlaybackRpcOwner> owner,
              ServiceId route,
              std::chrono::milliseconds cap,
              StoryId story,
              uint64_t start,
              uint64_t end,
              uint64_t max,
              std::shared_ptr<rpc::FanoutIssueBarrier> barrier)
            : barrier_(std::move(barrier))
            , owner_(std::move(owner))
            , route_(std::move(route))
            , cap_(cap)
            , story_(story)
            , start_(start)
            , end_(end)
            , max_(max)
        {}
        rpc::RpcResult<HotRangeResponse> run(rpc::OperationContext& operation) override
        {
            bool dispatched = false, arrived = false;
            struct Arrival
            {
                std::shared_ptr<rpc::FanoutIssueBarrier> barrier;
                rpc::RpcDeadline const& deadline;
                bool& arrived;
                ~Arrival()
                {
                    if(barrier && !arrived)
                        barrier->arriveAndWait(deadline);
                }
            } arrival{barrier_, operation.deadline(), arrived};
            try
            {
                if(operation.deadline().expired() || owner_->closing())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                auto binding = owner_->provisionHotFetch(route_.getProviderId(), operation.deadline());
                if(binding != CL_SUCCESS)
                    return {binding, false, std::nullopt};
                std::string address;
                route_.get_service_as_string(address);
                endpoint_.emplace(owner_->engine->lookup(address));
                peer_.emplace(*endpoint_, route_.getProviderId());
                call_.emplace(owner_->hotFetch.on(*peer_));
                auto timeout = operation.deadline().cappedAttempt(cap_);
                if(!timeout || owner_->closing() || !operation.tryBeginDispatch())
                    return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
                dispatched = true;
                pending_.emplace(call_->timed_async(*timeout, story_, start_, end_, max_));
                arrived = true;
                if(barrier_)
                    barrier_->arriveAndWait(operation.deadline());
                HotRangeResponse response = pending_->wait();
                pending_.reset();
                call_.reset();
                return {CL_SUCCESS, true, std::move(response)};
            }
            catch(...)
            {
                return {dispatched ? CL_ERR_OUTCOME_UNCONFIRMED : CL_ERR_UNKNOWN, dispatched, std::nullopt};
            }
        }

    private:
        std::shared_ptr<rpc::FanoutIssueBarrier> barrier_;
        std::shared_ptr<PlaybackRpcOwner> owner_;
        ServiceId route_;
        std::chrono::milliseconds cap_;
        StoryId story_;
        uint64_t start_, end_, max_;
        std::optional<thallium::endpoint> endpoint_;
        std::optional<thallium::provider_handle> peer_;
        using Call = decltype(std::declval<thallium::remote_procedure>().on(std::declval<thallium::provider_handle>()));
        std::optional<Call> call_;
        std::optional<thallium::async_response> pending_;
    };
    KeeperHotFetchClient(std::shared_ptr<PlaybackRpcOwner> owner, ServiceId route, std::chrono::milliseconds cap)
        : owner_(owner)
        , route_(std::move(route))
        , cap_(cap)
    {}
    std::weak_ptr<PlaybackRpcOwner> owner_;
    ServiceId route_;
    std::chrono::milliseconds cap_;
};
} // namespace chronolog
#endif
