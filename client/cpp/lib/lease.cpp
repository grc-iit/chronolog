#include "lease.h"
#include <cmath>
#include <ctime>
#include <limits>
#include <random>

namespace chronolog::client::detail
{
int64_t boottimeNs()
{
    timespec now{};
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &now);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif
    return static_cast<int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}
absl::Status forkedError()
{
    return absl::FailedPreconditionError("client used after fork; create a new Client in this process");
}
void Lease::confirm(const AcquisitionLease& granted, int64_t sent, const LeaseOptions& options)
{
    grant = granted;
    confirmed = true;
    expiry = sent + granted.remaining_ns;
    const auto margin = std::chrono::duration_cast<std::chrono::nanoseconds>(options.margin).count();
    const double lead = std::min(static_cast<double>(granted.duration_ns) * options.lead_fraction + margin,
                                 static_cast<double>(granted.duration_ns) * 0.9);
    due = expiry - static_cast<int64_t>(lead);
}
AcquireKey keyOf(StoryId story, const std::string& identity, const AcquireOptions& o)
{
    return {story,
            identity,
            o.lease_duration_ns,
            o.preferred_keeper_process_id,
            o.takeover,
            o.expected_prior_incarnation};
}
std::string Lifecycle::mint()
{
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device random;
    std::string id;
    do {
        id.clear();
        for(int word = 0; word < 4; ++word)
        {
            const uint32_t bits = random();
            for(int shift = 28; shift >= 0; shift -= 4) id.push_back(digits[(bits >> shift) & 0xf]);
        }
    } while(ids.contains(id));
    ids[id] = Id{Phase::Issued, {}, {}, ++clock};
    bound();
    return id;
}
void Lifecycle::bound()
{
    auto evict = [](auto& map, auto eligible)
    {
        auto victim = map.end();
        for(auto it = map.begin(); it != map.end(); ++it)
            if(eligible(it->second) && (victim == map.end() || it->second.touched < victim->second.touched))
                victim = it;
        if(victim == map.end())
            return false;
        map.erase(victim);
        return true;
    };
    while(ids.size() > max_ids &&
          (evict(ids, [](const Id& i) { return i.phase == Phase::Delivered || i.phase == Phase::Refused; }) ||
           evict(ids, [](const Id& i) { return i.phase != Phase::InFlight; })));
    while(priors.size() > max_priors && (evict(priors, [](const Prior& p) { return p.cause.has_value(); }) ||
                                         evict(priors, [](const Prior& p) { return p.writer.expired(); }) ||
                                         evict(priors, [](const Prior&) { return true; })));
}
void Lifecycle::terminal(const RenewAcquisition& t, AcquisitionTerminationCause cause)
{
    if(cause == AcquisitionTerminationCause::Unspecified)
        return;
    for(auto& [key, prior]: priors)
        if(key.first == t.story_id && prior.writer_id == t.writer_id && prior.incarnation == t.incarnation &&
           !prior.cause)
            prior.cause = cause;
}
void Lifecycle::releaseAttempted(StoryId story, const std::string& identity, uint64_t incarnation)
{
    auto found = priors.find({story, identity});
    if(found != priors.end() && found->second.incarnation == incarnation)
        found->second.release_attempted = true;
}
void Lifecycle::released(StoryId story, const std::string& identity, uint64_t incarnation)
{
    auto found = priors.find({story, identity});
    if(found != priors.end() && found->second.incarnation == incarnation)
        priors.erase(found);
}
absl::StatusOr<bool> release(State& state, const RenewAcquisition& t, TimePoint end)
{
    grpc::ClientContext context;
    withDeadline(context, end);
    v1::ReleaseRequest request;
    request.set_story_id(t.story_id);
    request.set_writer_id(t.writer_id);
    request.set_incarnation(t.incarnation);
    v1::ReleaseResponse response;
    auto transport = state.catalog->Release(&context, request, &response);
    if(!transport.ok())
        return status(transport);
    if(auto result = status(response.status()); !result.ok())
        return result;
    return response.fenced();
}
absl::StatusOr<v1::RenewAcquisitionsResponse>
renew(State& state, const std::vector<RenewAcquisition>& tuples, grpc::ClientContext& context)
{
    v1::RenewAcquisitionsRequest request;
    for(const auto& t: tuples)
    {
        auto* entry = request.add_acquisitions();
        entry->set_story_id(t.story_id);
        entry->set_writer_id(t.writer_id);
        entry->set_incarnation(t.incarnation);
    }
    v1::RenewAcquisitionsResponse response;
    auto transport = state.catalog->RenewAcquisitions(&context, request, &response);
    if(!transport.ok())
        return status(transport);
    if(response.results_size() != static_cast<int>(tuples.size()))
        return absl::DataLossError("renewal result count mismatch");
    for(size_t i = 0; i < tuples.size(); ++i)
    {
        const auto& echoed = response.results(static_cast<int>(i)).acquisition();
        if(echoed.story_id() != tuples[i].story_id || echoed.writer_id() != tuples[i].writer_id ||
           echoed.incarnation() != tuples[i].incarnation)
            return absl::DataLossError("renewal result order mismatch");
    }
    return response;
}
std::optional<AcquisitionTerminationCause> causeOf(const v1::RenewAcquisitionResult& result)
{
    if(!result.has_termination_cause())
        return std::nullopt;
    const auto cause = static_cast<uint32_t>(result.termination_cause());
    return cause <= static_cast<uint32_t>(AcquisitionTerminationCause::OwnerRemoved)
                   ? static_cast<AcquisitionTerminationCause>(cause)
                   : AcquisitionTerminationCause::Unspecified;
}
Scheduler::Scheduler(std::shared_ptr<State> state)
    : state_(std::move(state))
{}
Scheduler::~Scheduler()
{
    if(state_->forked())
    {
        // The thread does not exist in a forked child and its locks may be held forever.
        if(thread_.joinable())
            thread_.detach();
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        if(call_)
            call_->TryCancel();
    }
    cv_.notify_all();
    if(thread_.joinable())
        thread_.join();
}
void Scheduler::add(std::weak_ptr<Lease> lease)
{
    {
        std::lock_guard lock(mutex_);
        leases_.push_back(std::move(lease));
        if(!thread_.joinable() && !stop_)
            thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
}
void Scheduler::run()
{
    const auto& options = state_->options.lease;
    const auto poll = std::chrono::duration_cast<std::chrono::nanoseconds>(options.poll).count();
    std::unique_lock lock(mutex_);
    while(!stop_)
    {
        const auto now = state_->boottime();
        std::vector<std::shared_ptr<Lease>> batch;
        int64_t next = std::numeric_limits<int64_t>::max();
        std::erase_if(leases_,
                      [&](const std::weak_ptr<Lease>& weak)
                      {
                          auto lease = weak.lock();
                          if(!lease)
                              return true;
                          std::lock_guard lease_lock(lease->mutex);
                          if(!lease->renewable())
                              return true;
                          if(now >= lease->expiry)
                              lease->confirmed = false;
                          if(now >= lease->due && batch.size() < options.max_batch)
                              batch.push_back(lease);
                          else
                              next = std::min(next, lease->due);
                          return false;
                      });
        if(batch.empty())
        {
            const auto wait =
                    next == std::numeric_limits<int64_t>::max() ? poll : std::clamp<int64_t>(next - now, 0, poll);
            cv_.wait_for(lock, std::chrono::nanoseconds(wait));
            continue;
        }
        std::vector<RenewAcquisition> tuples;
        for(const auto& lease: batch) tuples.push_back(lease->tuple);
        grpc::ClientContext context;
        withDeadline(context, std::chrono::system_clock::now() + state_->options.rpc_timeout);
        call_ = &context;
        lock.unlock();
        const auto sent = state_->boottime();
        auto response = renew(*state_, tuples, context);
        const auto after = state_->boottime();
        lock.lock();
        call_ = nullptr;
        std::vector<std::pair<RenewAcquisition, AcquisitionTerminationCause>> terminal;
        for(size_t i = 0; i < batch.size(); ++i)
        {
            auto& lease = *batch[i];
            std::lock_guard lease_lock(lease.mutex);
            if(lease.closing || lease.cause)
                continue;
            if(response.ok())
            {
                const auto& result = response->results(static_cast<int>(i));
                const auto code = static_cast<absl::StatusCode>(result.status().code());
                if(code == absl::StatusCode::kOk && result.lease().duration_ns() > 0 &&
                   result.lease().remaining_ns() >= 0)
                {
                    lease.confirm({result.lease().duration_ns(), result.lease().remaining_ns()}, sent, options);
                    ++lease.renewals;
                    continue;
                }
                if(code == absl::StatusCode::kFailedPrecondition)
                {
                    lease.cause = causeOf(result).value_or(AcquisitionTerminationCause::Unspecified);
                    lease.confirmed = false;
                    terminal.emplace_back(lease.tuple, *lease.cause);
                    continue;
                }
            }
            // Outage, NOT_FOUND or a malformed answer: unconfirmed diagnostic, never a fence.
            lease.confirmed = false;
            lease.due = after + poll;
        }
        if(!terminal.empty())
        {
            std::lock_guard life_lock(state_->lifecycle->mutex);
            for(const auto& [tuple, cause]: terminal) state_->lifecycle->terminal(tuple, cause);
        }
    }
}
} // namespace chronolog::client::detail
