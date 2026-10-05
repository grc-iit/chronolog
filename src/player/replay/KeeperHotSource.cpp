#include "common/rpc/Channel.h"
#include "player/replay/KeeperHotSource.h"
#include <future>
#include <algorithm>
#include <limits>
#include <thread>
#include <absl/log/log.h>
#include <grpcpp/grpcpp.h>
#include "player/adapter/Convert.h"
#include "player/replay/PhysicalRead.h"

namespace chronolog::player
{

KeeperHotSource::KeeperHotSource(std::shared_ptr<const RouteSource> routes,
                                 std::shared_ptr<const WriterSource> writers,
                                 std::function<std::string(const KeeperRef&)> internal_address,
                                 KeeperHotSourceOptions options)
    : routes_(std::move(routes))
    , writers_(std::move(writers))
    , internal_address_(std::move(internal_address))
    , options_(options)
{}

void KeeperHotSource::warm(const std::string& address) const { (void)stubFor(address); }

std::shared_ptr<internal::v1::Archive::Stub> KeeperHotSource::stubFor(const std::string& address) const
{
    std::lock_guard lk(mu_);
    auto& stub = stubs_[address];
    if(!stub)
    {
        stub = internal::v1::Archive::NewStub(rpc::peerChannel(address));
    }
    return stub;
}

KeeperFetch KeeperHotSource::fetchOne(const KeeperRef& keeper,
                                      StoryId story,
                                      const Range& range,
                                      Epoch expected_epoch,
                                      const Predecessor* predecessor,
                                      std::atomic<size_t>& retained,
                                      bool policy,
                                      bool tail,
                                      size_t read_budget,
                                      const EventPredicate& predicate,
                                      std::chrono::system_clock::time_point deadline) const
{
    KeeperFetch out;
    out.frontier.process_id = keeper.process_id;
    out.frontier.answered = false;
    out.frontier.expected_epoch = expected_epoch;
    out.frontier.predecessor = predecessor != nullptr;
    if(predecessor)
    {
        out.frontier.own_cut = predecessor->own_cut;
        out.frontier.instance = predecessor->instance;
    }

    auto scan = range.axis == Range::Axis::Physical ? physicalWindow(range, policy) : range;
    if(predecessor)
    {
        scan.end = std::min(scan.end, predecessor->own_cut);
        scan.start = std::min(scan.start, scan.end);
    }
    // A Tail round must make progress when every source is truncated, which needs two events per answer (I6.13).
    const uint64_t max_events = read_budget ? read_budget
                                            : (tail && options_.max_events ? std::max<uint64_t>(options_.max_events, 2)
                                                                           : options_.max_events);
    const size_t budget = read_budget
                                  ? read_budget
                                  : (tail ? std::max<size_t>(options_.read_max_events, 2) : options_.read_max_events);
    auto request = convert::fetchHotRequest(story, scan, max_events);
    if(range.axis == Range::Axis::Physical)
    {
        request.mutable_physical_filter()->set_start_ns(range.start.physical_ns);
        request.mutable_physical_filter()->set_end_ns(range.end.physical_ns);
    }
    request.set_expect_epoch(expected_epoch);
    if(!predicate.empty())
        *request.mutable_predicate() = convert::toProto(predicate);
    if(predecessor)
        request.set_expect_instance(predecessor->instance);
    const auto address = internal_address_(keeper);
    do {
        grpc::ClientContext context;
        rpc::withDeadline(context, deadline);
        auto stub = stubFor(address);
        auto reader = stub->FetchHot(&context, request);
        internal::v1::FetchHotResponse response;
        bool received = false;
        bool trailer = false;
        bool limited = false;
        bool instance_matches = !predecessor;
        while(reader->Read(&response))
        {
            received = true;
            if(response.has_batch())
            {
                for(auto& event: *response.mutable_batch()->mutable_events())
                {
                    if(limited)
                        continue;
                    size_t count = retained.load();
                    while(count < budget && !retained.compare_exchange_weak(count, count + 1)) {}
                    if(count < budget)
                        out.events.push_back(convert::fromProto(std::move(event)));
                    else
                        limited = true;
                }
            }
            else if(response.has_trailer())
            {
                trailer = true;
                out.frontier.instance = response.trailer().instance();
                instance_matches = !predecessor || response.trailer().instance() == predecessor->instance;
                if(response.trailer().has_physical_frontier_ns())
                    out.frontier.physical_frontier = response.trailer().physical_frontier_ns();
                out.frontier.epoch = response.trailer().epoch();
                out.frontier.sealed = convert::fromProto(response.trailer().sealed_frontier());
                out.frontier.evicted_below = convert::fromProto(response.trailer().evicted_below());
                out.frontier.truncated = response.trailer().truncated();
            }
        }
        const auto status = reader->Finish();
        out.frontier.status = static_cast<absl::StatusCode>(status.error_code());
        out.frontier.truncated |= limited;
        out.frontier.answered = status.ok() && trailer && instance_matches && out.frontier.epoch == expected_epoch;
        if(!out.frontier.answered)
            LOG(WARNING) << "fetch_hot_failed keeper=" << keeper.process_id << " status=" << status.error_code()
                         << " trailer=" << trailer << " message=" << status.error_message();
        // Retry readiness failures without replaying a partially received stream or extending the deadline.
        if(received || status.error_code() != grpc::StatusCode::UNAVAILABLE)
            break;
        std::this_thread::sleep_until(
                std::min(deadline, std::chrono::system_clock::now() + std::chrono::milliseconds(50)));
    } while(std::chrono::system_clock::now() < deadline);
    return out;
}

absl::StatusOr<HotFetch> KeeperHotSource::fetch(StoryId story, const Range& range) const
{
    return fetchImpl(story, range, true, nullptr, {});
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchRead(StoryId story, const Range& range, size_t target) const
{
    return fetchReadMatching(story, range, target, {});
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchTail(StoryId story, Hlc from, const TailStarts& starts) const
{
    return fetchTailMatching(story, from, starts, {});
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchPhysical(StoryId story, const Range& range, bool policy) const
{
    return fetchPhysicalMatching(story, range, policy, {});
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchReadMatching(StoryId story,
                                                            const Range& range,
                                                            size_t target,
                                                            const EventPredicate& predicate) const
{
    const size_t budget = target == SIZE_MAX ? target : target + 1;
    return fetchImpl(story, range, true, nullptr, predicate, budget);
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchTailMatching(StoryId story,
                                                            Hlc from,
                                                            const TailStarts& starts,
                                                            const EventPredicate& predicate) const
{
    return fetchImpl(story, Range{Range::Axis::Hlc, from, maxHlc()}, true, &starts, predicate);
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchPhysicalMatching(StoryId story,
                                                                const Range& range,
                                                                bool policy,
                                                                const EventPredicate& predicate) const
{
    return fetchImpl(story, range, policy, nullptr, predicate);
}

absl::StatusOr<HotFetch> KeeperHotSource::fetchImpl(StoryId story,
                                                    const Range& range,
                                                    bool policy,
                                                    const TailStarts* starts,
                                                    const EventPredicate& predicate,
                                                    size_t read_budget) const
{
    const auto deadline = std::chrono::system_clock::now() + options_.deadline;
    auto state = routes_->routeState(story);
    if(!state.ok())
        return state.status();
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        HotFetch out;
        out.route_epoch = state->route.epoch;
        out.archived_below = state->archived_below;
        out.abandoned = state->abandoned;
        out.physical_policy = policy && routes_->physicalPolicy(story);
        std::atomic<size_t> retained{0};
        // A Tail round gives every source its own start and its own budget.
        const bool tail = starts != nullptr;
        auto startOf = [&](const SourceId& id)
        {
            if(tail)
                if(auto it = starts->find(id); it != starts->end())
                    return std::max(it->second, range.start);
            return range.start;
        };
        auto useRetained = [&](const SourceId& id, Epoch epoch, const Predecessor* predecessor)
        {
            if(!tail)
                return false;
            const auto it = starts->retained.find(id);
            if(it == starts->retained.end() || it->second.epoch != epoch ||
               (predecessor && it->second.instance != predecessor->instance))
                return false;
            auto frontier = it->second;
            if(predecessor)
                frontier.own_cut = predecessor->own_cut;
            out.keepers.push_back({std::move(frontier), {}});
            return true;
        };
        std::vector<std::future<KeeperFetch>> pending;
        for(const auto& keeper: state->route.keepers)
        {
            if(useRetained({keeper.process_id, 0}, state->route.epoch, nullptr))
                continue;
            Range own = range;
            own.start = startOf({keeper.process_id, 0});
            pending.push_back(std::async(std::launch::async,
                                         [&, keeper, own]
                                         {
                                             std::atomic<size_t> mine{0};
                                             return fetchOne(keeper,
                                                             story,
                                                             own,
                                                             state->route.epoch,
                                                             nullptr,
                                                             tail || read_budget ? mine : retained,
                                                             out.physical_policy,
                                                             tail,
                                                             read_budget,
                                                             predicate,
                                                             deadline);
                                         }));
        }
        for(const auto& p: state->predecessors)
        {
            if(useRetained({p.keeper.process_id, p.epoch}, p.epoch, &p))
                continue;
            Range own = range;
            if(range.axis == Range::Axis::Hlc)
            {
                own.start = startOf({p.keeper.process_id, p.epoch});
                if(own.start >= p.own_cut)
                    continue;
                own.end = std::min(range.end, p.own_cut);
            }
            else if(out.physical_policy)
            {
                int64_t bound = std::max(p.own_cut.physical_ns, p.own_physical_ceiling_ns);
                int64_t skew = std::max<int64_t>(0, routes_->skewLimitNs());
                bound = bound > std::numeric_limits<int64_t>::max() - skew ? std::numeric_limits<int64_t>::max()
                                                                           : bound + skew;
                if(range.start.physical_ns >= bound)
                    continue;
            }
            pending.push_back(std::async(std::launch::async,
                                         [&, p, own]
                                         {
                                             std::atomic<size_t> mine{0};
                                             return fetchOne(p.keeper,
                                                             story,
                                                             own,
                                                             p.epoch,
                                                             &p,
                                                             tail || read_budget ? mine : retained,
                                                             out.physical_policy,
                                                             tail,
                                                             read_budget,
                                                             predicate,
                                                             deadline);
                                         }));
        }
        for(auto& f: pending)
        {
            auto answer = f.get();
            if(answer.frontier.truncated)
            {
                answer.frontier.truncated_at = answer.events.empty() ? range.start : answer.events.back().hlc;
            }
            if(answer.frontier.predecessor && range.axis == Range::Axis::Hlc)
                std::erase_if(answer.events, [&](const Event& e) { return e.hlc >= answer.frontier.own_cut; });
            out.keepers.push_back(std::move(answer));
        }
        if(writers_)
            out.writers = writers_->writers(story);
        const bool stale = std::any_of(out.keepers.begin(),
                                       out.keepers.end(),
                                       [](const auto& k)
                                       {
                                           return !k.frontier.predecessor && !k.frontier.answered &&
                                                  (k.frontier.status == absl::StatusCode::kFailedPrecondition ||
                                                   (k.frontier.status == absl::StatusCode::kOk &&
                                                    k.frontier.epoch != k.frontier.expected_epoch));
                                       });
        if(!tail && attempt == 0 && stale && std::chrono::system_clock::now() < deadline)
        {
            auto next = routes_->routeStateAfter(story, state->route.epoch, deadline);
            if(!next.ok())
            {
                // The Read already captured its sources; a reconnect tombstone cannot erase their failure evidence.
                if(absl::IsFailedPrecondition(next.status()))
                    return out;
                return next.status();
            }
            if(next->route.epoch > state->route.epoch)
            {
                state = std::move(next);
                continue;
            }
        }
        return out;
    }
    return absl::InternalError("route retry exhausted");
}

} // namespace chronolog::player
