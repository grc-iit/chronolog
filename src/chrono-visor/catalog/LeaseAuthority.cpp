#include "catalog/LeaseAuthority.h"

#include <algorithm>
#include <limits>
#include <random>
#include "chronolog/acquire_refusal.h"

namespace chronolog::visor
{
absl::Status AcquisitionLeaseConfig::validate(uint32_t election, uint32_t heartbeat, uint32_t fence) const
{
    const uint64_t floor_ms = uint64_t(std::max(election, heartbeat)) + fence + lease_safety_margin_ms;
    if(acquisition_lease_min_ns <= 0 || acquisition_lease_default_ns < acquisition_lease_min_ns ||
       acquisition_lease_max_ns < acquisition_lease_default_ns || acquisition_lease_max_ns > INT64_MAX / 2 ||
       uint64_t(acquisition_lease_min_ns) <= floor_ms * 1000000 || !acquisition_service_tick_ms ||
       acquisition_service_tick_ms >= acquisition_service_gap_ms ||
       acquisition_service_gap_ms > lease_safety_margin_ms ||
       uint64_t(acquisition_service_gap_ms) * 1000000 >= uint64_t(acquisition_lease_min_ns) ||
       !acquisition_scan_batch || !acquisition_expiry_batch || !acquisition_renew_batch ||
       !acquisition_evidence_batch || acquisition_scan_batch > 65536 || acquisition_expiry_batch > 65536 ||
       acquisition_renew_batch > 65536 || acquisition_evidence_batch > 65536)
        return absl::InvalidArgumentError("invalid acquisition lease configuration");
    return absl::OkStatus();
}
absl::StatusOr<int64_t> AcquisitionLeaseConfig::duration(const AcquireOptions& o) const
{
    if((o.lease_duration_ns && *o.lease_duration_ns <= 0) ||
       (o.preferred_keeper_process_id && o.preferred_keeper_process_id->empty()) ||
       (o.expected_prior_incarnation && !*o.expected_prior_incarnation) || o.acquire_request_id.empty() ||
       o.acquire_request_id.size() > 128)
        return absl::InvalidArgumentError("invalid acquisition options");
    // Reject malformed UTF-8 and control bytes before any mutation.
    const auto& s = o.acquire_request_id;
    for(size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i++]);
        if(c < 0x20 || c == 0x7f)
            return absl::InvalidArgumentError("invalid request id");
        if(c < 0x80)
            continue;
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if(!n || i + n > s.size())
            return absl::InvalidArgumentError("invalid request id");
        const auto first = static_cast<unsigned char>(s[i]);
        if((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) || (c == 0xf0 && first < 0x90) ||
           (c == 0xf4 && first >= 0x90))
            return absl::InvalidArgumentError("invalid request id");
        while(n--)
            if((static_cast<unsigned char>(s[i++]) & 0xc0) != 0x80)
                return absl::InvalidArgumentError("invalid request id");
    }
    return o.lease_duration_ns ? std::clamp(*o.lease_duration_ns, acquisition_lease_min_ns, acquisition_lease_max_ns)
                               : acquisition_lease_default_ns;
}
std::string newAcquireRequestId()
{
    std::random_device random;
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for(int i = 0; i < 4; ++i)
    {
        auto value = random();
        for(int j = 0; j < 8; ++j)
        {
            out.push_back(hex[value & 15]);
            value >>= 4;
        }
    }
    return out;
}
std::string acquireInputs(StoryId id, const std::string& identity, const AcquireOptions& o)
{
    std::string out = std::to_string(id) + ":" + std::to_string(identity.size()) + ":" + identity;
    out += ":" + (o.lease_duration_ns ? std::to_string(*o.lease_duration_ns) : "absent");
    out += ":" + std::to_string(o.takeover) + ":" +
           (o.expected_prior_incarnation ? std::to_string(*o.expected_prior_incarnation) : "absent");
    out += ":" + (o.preferred_keeper_process_id ? "present:" + *o.preferred_keeper_process_id : "absent");
    return out;
}
absl::Status priorMismatch(std::optional<uint64_t> current, std::optional<uint64_t> matched)
{
    auto status = absl::FailedPreconditionError("prior incarnation mismatch");
    AcquireRefusal detail;
    detail.refusal_reason = AcquireRefusalReason::PriorMismatch;
    detail.current_incarnation = current;
    detail.matched_incarnation = matched;
    setAcquireRefusal(status, detail);
    return status;
}
absl::Status terminalRetry(uint64_t matched, AcquisitionTerminationCause cause)
{
    auto status = absl::FailedPreconditionError("acquisition is terminal");
    AcquireRefusal detail;
    detail.matched_incarnation = matched;
    detail.termination_cause = cause;
    setAcquireRefusal(status, detail);
    return status;
}
absl::Status validateRenew(const std::vector<RenewAcquisition>& tuples, size_t limit)
{
    if(tuples.empty() || tuples.size() > limit)
        return absl::InvalidArgumentError("invalid renewal batch size");
    for(const auto& t: tuples)
        if(!t.story_id || !t.writer_id || !t.incarnation)
            return absl::InvalidArgumentError("invalid renewal tuple");
    return absl::OkStatus();
}
LeaseAuthority::LeaseAuthority(AcquisitionLeaseConfig config, bool dynamic)
    : config_(config)
    , dynamic_(dynamic)
    , last_tick_(now())
{
    qualification_.store(std::make_shared<LeaseQualification>(LeaseQualification{0, last_tick_, !dynamic}));
}
int64_t LeaseAuthority::now() const
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count() +
           clock_offset_.load();
}
void LeaseAuthority::publish(LeaseQualification q) { qualification_.store(std::make_shared<LeaseQualification>(q)); }
absl::Status LeaseAuthority::serviceLocked(int64_t time, bool tick)
{
    const auto q = qualification_.load();
    if(time - last_tick_ > int64_t(config_.acquisition_service_gap_ms) * 1000000)
        lapse_start_ = lapse_start_ ? std::min(*lapse_start_, last_tick_) : last_tick_;
    if(!q->qualified || (dynamic_ && q->term != term_) || rebuilding_)
    {
        if(!lapse_start_)
            lapse_start_ = time;
        if(tick)
            last_tick_ = time;
        return absl::UnavailableError("acquisition authority is not qualified");
    }
    if(lapse_start_)
    {
        if(dynamic_ && q->sampled_ns < *lapse_start_)
            return absl::UnavailableError("stale acquisition authority sample");
        const int64_t pause = time - *lapse_start_;
        for(auto& [key, entry]: entries_)
            if(entry.deadline)
                entry.deadline += pause;
        lapse_start_.reset();
    }
    for(auto& [key, entry]: entries_)
        if(!entry.deadline)
            entry.deadline = time + entry.row.duration_ns;
    if(tick)
        last_tick_ = time;
    return absl::OkStatus();
}
absl::Status LeaseAuthority::service(bool tick)
{
    std::lock_guard lock(mutex_);
    return serviceLocked(now(), tick);
}
void LeaseAuthority::beginRebuild(uint64_t term)
{
    std::lock_guard lock(mutex_);
    rebuilding_ = true;
    term_ = term;
    buffered_.clear();
}
void LeaseAuthority::rebuild(const AcquisitionSnapshot& snapshot)
{
    std::lock_guard lock(mutex_);
    entries_.clear();
    revisions_.clear();
    lapse_start_.reset();
    const auto time = now();
    last_tick_ = time;
    for(const auto& row: snapshot.active) applyLocked(row, time, true);
    for(const auto& row: buffered_)
        if(row.revision > snapshot.revision)
            applyLocked(row, time, true);
    buffered_.clear();
    rebuilding_ = false;
}
void LeaseAuthority::applyLocked(const AcquisitionChange& row, int64_t time, bool qualified)
{
    const auto slot = std::pair{row.story_id, row.writer_id};
    if(revisions_[slot] > row.revision)
        return;
    revisions_[slot] = row.revision;
    const Key key{row.story_id, row.writer_id, row.incarnation};
    if(row.state == AcquisitionState::Released)
    {
        entries_.erase(key);
        return;
    }
    if(!entries_.contains(key))
        entries_[key] = {row, qualified ? time + row.duration_ns : 0};
}
void LeaseAuthority::onAcquisitionChange(const AcquisitionChange& row)
{
    std::lock_guard lock(mutex_);
    if(rebuilding_)
    {
        buffered_.push_back(row);
        return;
    }
    const auto qualification = qualification_.load();
    if(dynamic_ && !qualification->qualified)
    {
        if(row.state == AcquisitionState::Released)
            applyLocked(row, 0, false);
        return;
    }
    const auto time = now();
    const bool qualified = serviceLocked(time, false).ok();
    applyLocked(row, time, qualified);
}
void LeaseAuthority::reconcile(const AcquisitionSnapshot& snapshot, std::pair<StoryId, uint64_t> after, bool end)
{
    std::lock_guard lock(mutex_);
    const auto time = now();
    if(!serviceLocked(time, false).ok())
        return;
    auto through = end ? std::pair{UINT64_MAX, UINT64_MAX}
                       : std::pair{snapshot.active.back().story_id, snapshot.active.back().writer_id};
    for(auto it = entries_.begin(); it != entries_.end();)
    {
        const auto slot = std::pair{std::get<0>(it->first), std::get<1>(it->first)};
        const bool present = std::any_of(snapshot.active.begin(),
                                         snapshot.active.end(),
                                         [&](const auto& row)
                                         { return it->first == Key{row.story_id, row.writer_id, row.incarnation}; });
        if(slot > after && slot <= through && revisions_[slot] <= snapshot.revision && !present)
            it = entries_.erase(it);
        else
            ++it;
    }
    for(const auto& row: snapshot.active) applyLocked(row, time, true);
}
absl::StatusOr<AcquisitionLease> LeaseAuthority::sample(const AcquisitionChange& row, bool renew)
{
    std::lock_guard lock(mutex_);
    const auto time = now();
    auto status = serviceLocked(time, false);
    if(!status.ok())
        return status;
    const auto slot = std::pair{row.story_id, row.writer_id};
    const Key key{row.story_id, row.writer_id, row.incarnation};
    if(revisions_[slot] > row.revision && !entries_.contains(key))
        return absl::UnavailableError("acquisition changed during renewal");
    applyLocked(row, time, true);
    auto it = entries_.find(key);
    if(it == entries_.end())
        return absl::FailedPreconditionError("acquisition is terminal");
    auto& entry = it->second;
    if(renew)
    {
        if(time >= entry.deadline)
            return absl::UnavailableError("acquisition deadline is due");
        entry.deadline = std::max(entry.deadline, time + row.duration_ns);
    }
    return AcquisitionLease{row.duration_ns, std::max<int64_t>(0, entry.deadline - time)};
}
void LeaseAuthority::advanceClock(int64_t ns, bool ticking)
{
    if(ns < 0 || ns > INT64_MAX / 4)
        throw std::invalid_argument("invalid clock advance");
    if(!ticking)
    {
        clock_offset_.fetch_add(ns);
        return;
    }
    const auto step = int64_t(config_.acquisition_service_tick_ms) * 1000000;
    for(int64_t left = ns; left > 0;)
    {
        auto amount = std::min(step, left);
        clock_offset_.fetch_add(amount);
        left -= amount;
        auto q = *qualification_.load();
        q.sampled_ns = now();
        publish(q);
        (void)service(true);
    }
}
void LeaseAuthority::eraseForTest(RenewAcquisition t)
{
    std::lock_guard lock(mutex_);
    entries_.erase({t.story_id, t.writer_id, t.incarnation});
}
size_t LeaseAuthority::size() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}
} // namespace chronolog::visor
