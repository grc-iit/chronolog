#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include "catalog/AcquisitionLedger.h"

namespace chronolog::visor
{
struct AcquisitionLeaseConfig
{
    int64_t acquisition_lease_default_ns = 300000000000;
    int64_t acquisition_lease_min_ns = 30000000000;
    int64_t acquisition_lease_max_ns = 3600000000000;
    uint32_t lease_safety_margin_ms = 5000;
    uint32_t acquisition_service_tick_ms = 100;
    uint32_t acquisition_service_gap_ms = 1000;
    uint32_t acquisition_scan_batch = 256;
    uint32_t acquisition_expiry_batch = 256;
    uint32_t acquisition_renew_batch = 256;
    uint32_t acquisition_evidence_batch = 256;
    absl::Status
    validate(uint32_t election_upper_ms, uint32_t heartbeat_timeout_ms, uint32_t release_fence_timeout_ms) const;
    absl::StatusOr<int64_t> duration(const AcquireOptions& options) const;
};

std::string newAcquireRequestId();
std::string acquireInputs(StoryId id, const std::string& identity, const AcquireOptions& options);
absl::Status priorMismatch(std::optional<uint64_t> current, std::optional<uint64_t> matched = {});
absl::Status terminalRetry(uint64_t matched, AcquisitionTerminationCause cause);
absl::Status validateRenew(const std::vector<RenewAcquisition>& tuples, size_t limit);

struct LeaseQualification
{
    uint64_t term{};
    int64_t sampled_ns{};
    bool qualified{};
};

class LeaseAuthority final: public AcquisitionObserver
{
public:
    explicit LeaseAuthority(AcquisitionLeaseConfig config = {}, bool dynamic = false);
    int64_t now() const;
    void publish(LeaseQualification qualification);
    absl::Status service(bool completed_tick = false);
    void beginRebuild(uint64_t term);
    void rebuild(const AcquisitionSnapshot& snapshot);
    void reconcile(const AcquisitionSnapshot& snapshot, std::pair<StoryId, uint64_t> after, bool end);
    void onAcquisitionChange(const AcquisitionChange& change) override;
    absl::StatusOr<AcquisitionLease> sample(const AcquisitionChange& row, bool renew);
    void advanceClock(int64_t ns, bool ticking);
    void eraseForTest(RenewAcquisition tuple);
    std::vector<RenewAcquisition> reconciliationTuples();
    void reconcileTerminals(const std::vector<AcquisitionChange>& rows);
    absl::StatusOr<std::vector<RenewAcquisition>> dueTuples(size_t limit);
    size_t size() const;
    const AcquisitionLeaseConfig& config() const { return config_; }

private:
    using Key = std::tuple<StoryId, uint64_t, uint64_t>;
    struct Entry
    {
        AcquisitionChange row;
        int64_t deadline{};
    };
    absl::Status serviceLocked(int64_t now, bool completed_tick);
    void applyLocked(const AcquisitionChange& change, int64_t now, bool qualified);
    void removeLocked(const Key& key);
    AcquisitionLeaseConfig config_;
    bool dynamic_{};
    std::atomic<int64_t> clock_offset_{};
    std::atomic<std::shared_ptr<const LeaseQualification>> qualification_;
    mutable std::mutex mutex_;
    std::map<Key, Entry> entries_;
    std::set<std::pair<int64_t, Key>> due_;
    std::set<Key> deferred_initialization_;
    std::map<std::pair<StoryId, uint64_t>, uint64_t> revisions_;
    std::optional<int64_t> lapse_start_;
    int64_t last_tick_{};
    int64_t gap_accounted_through_{};
    uint64_t term_{};
    bool rebuilding_{};
    std::vector<AcquisitionChange> buffered_;
    Key reconciliation_cursor_{};
};
} // namespace chronolog::visor
