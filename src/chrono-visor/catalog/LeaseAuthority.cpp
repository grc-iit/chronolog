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
    if(acquisition_lease_min_ns <= 0 || acquisition_lease_default_ns < acquisition_lease_min_ns ||
       acquisition_lease_max_ns < acquisition_lease_default_ns || acquisition_lease_max_ns > INT64_MAX / 2)
        return absl::InvalidArgumentError("invalid finite lease bounds");
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
void LeaseAuthority::publish(LeaseQualification q)
{
    auto prior = qualification_.load();
    auto next = std::make_shared<const LeaseQualification>(q);
    while(q.sampled_ns >= prior->sampled_ns && q.term >= prior->term)
        if(qualification_.compare_exchange_weak(prior, next))
            return;
}
absl::Status LeaseAuthority::serviceLocked(int64_t time, bool tick)
{
    const auto q = qualification_.load();
    if(time - last_tick_ > int64_t(config_.acquisition_service_gap_ms) * 1000000)
    {
        const auto start = std::max(last_tick_, gap_accounted_through_);
        lapse_start_ = lapse_start_ ? std::min(*lapse_start_, start) : start;
    }
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
            {
                due_.erase({entry.deadline, key});
                entry.deadline += pause;
                due_.insert({entry.deadline, key});
            }
        lapse_start_.reset();
        gap_accounted_through_ = time;
        last_tick_ = time;
    }
    for(const auto& key: deferred_initialization_)
    {
        auto& entry = entries_.at(key);
        entry.deadline = time + entry.row.duration_ns;
        due_.insert({entry.deadline, key});
    }
    deferred_initialization_.clear();
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
    due_.clear();
    deferred_initialization_.clear();
    revisions_.clear();
    lapse_start_.reset();
    const auto time = now();
    last_tick_ = time;
    gap_accounted_through_ = time;
    for(const auto& row: snapshot.active) applyLocked(row, time, true);
    for(const auto& row: buffered_)
        if(row.applied_index ? row.applied_index > snapshot.applied_index : row.revision > snapshot.revision)
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
        removeLocked(key);
        return;
    }
    if(!entries_.contains(key))
    {
        entries_[key] = {row, qualified ? time + row.duration_ns : 0};
        if(qualified)
            due_.insert({entries_[key].deadline, key});
        else
            deferred_initialization_.insert(key);
    }
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
    (void)after;
    (void)end;
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
        due_.erase({entry.deadline, key});
        entry.deadline = std::max(entry.deadline, time + row.duration_ns);
        due_.insert({entry.deadline, key});
    }
    return AcquisitionLease{row.duration_ns, std::max<int64_t>(0, entry.deadline - time)};
}
size_t LeaseAuthority::acceptEvidence(const std::string& keeper, const std::vector<AcquisitionChange>& rows)
{
    size_t renewed = 0;
    for(const auto& row: rows)
        if(row.state == AcquisitionState::Acquired && row.assigned_keeper.process_id == keeper &&
           sample(row, true).ok())
            ++renewed;
    return renewed;
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
    auto initial = *qualification_.load();
    initial.sampled_ns = now();
    publish(initial);
    (void)service(true);
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
std::vector<RenewAcquisition> LeaseAuthority::reconciliationTuples()
{
    std::lock_guard lock(mutex_);
    std::vector<RenewAcquisition> tuples;
    auto it = entries_.upper_bound(reconciliation_cursor_);
    if(it == entries_.end())
    {
        reconciliation_cursor_ = {};
        it = entries_.begin();
    }
    for(size_t count = 0; it != entries_.end() && count < config_.acquisition_scan_batch; ++it, ++count)
    {
        reconciliation_cursor_ = it->first;
        tuples.push_back({std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first)});
    }
    return tuples;
}
void LeaseAuthority::reconcileTerminals(const std::vector<AcquisitionChange>& rows)
{
    std::lock_guard lock(mutex_);
    for(const auto& row: rows)
    {
        if(row.state != AcquisitionState::Released)
            continue;
        auto entry = entries_.find({row.story_id, row.writer_id, row.incarnation});
        if(entry != entries_.end() && (row.termination_cause != AcquisitionTerminationCause::Unspecified ||
                                       entry->second.row.revision <= row.revision))
            removeLocked(entry->first);
    }
}
void LeaseAuthority::eraseForTest(RenewAcquisition t)
{
    std::lock_guard lock(mutex_);
    removeLocked({t.story_id, t.writer_id, t.incarnation});
}
void LeaseAuthority::removeLocked(const Key& key)
{
    auto it = entries_.find(key);
    if(it == entries_.end())
        return;
    due_.erase({it->second.deadline, key});
    deferred_initialization_.erase(key);
    entries_.erase(it);
}
absl::StatusOr<std::vector<RenewAcquisition>> LeaseAuthority::dueTuples(size_t limit)
{
    std::lock_guard lock(mutex_);
    const auto time = now();
    auto status = serviceLocked(time, false);
    if(!status.ok())
        return status;
    std::vector<RenewAcquisition> tuples;
    for(auto it = due_.begin(); it != due_.end() && tuples.size() < limit && it->first <= time; ++it)
        tuples.push_back({std::get<0>(it->second), std::get<1>(it->second), std::get<2>(it->second)});
    return tuples;
}
size_t LeaseAuthority::size() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}
} // namespace chronolog::visor
