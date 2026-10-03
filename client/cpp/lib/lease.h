#pragma once
#include "internal.h"
#include <memory>

namespace chronolog::client::detail
{
absl::Status forkedError();
// Catalog-confirmed grant estimate for one Writer, owned by Writer::Impl and weakly held by the scheduler,
// so a moved Writer keeps its registration and a dropped one leaves it.
struct Lease
{
    explicit Lease(RenewAcquisition t)
        : tuple(t)
    {}
    const RenewAcquisition tuple;
    mutable std::mutex mutex;
    AcquisitionLease grant;
    // Local boottime at which the grant lapses, charged from the request send time.
    int64_t expiry{};
    int64_t due{};
    bool confirmed{true};
    bool closing{};
    std::optional<AcquisitionTerminationCause> cause;
    uint64_t renewals{};
    // Callers hold mutex.
    void confirm(const AcquisitionLease& granted, int64_t sent, const LeaseOptions& options);
    bool renewable() const { return !closing && !cause && grant.duration_ns > 0; }
};
struct AcquireKey
{
    StoryId story{};
    std::string identity;
    std::optional<int64_t> lease_duration_ns;
    std::optional<std::string> preferred_keeper_process_id;
    bool takeover{};
    std::optional<uint64_t> expected_prior_incarnation;
    bool operator==(const AcquireKey&) const = default;
};
AcquireKey keyOf(StoryId, const std::string& identity, const AcquireOptions&);
// Process-local acquire request ids (C8) and this Client's own unreleased incarnations (RFC-G section 7).
struct Lifecycle
{
    enum class Phase
    {
        Issued,
        InFlight,
        // Returned without a grant after a dispatch whose outcome is uncertain; an identical call reuses it.
        Unresolved,
        // Returned without a grant after a definitive answer; reusable only when named explicitly.
        Refused,
        Delivered
    };
    struct Id
    {
        Phase phase{Phase::Issued};
        std::optional<AcquireKey> key;
        std::optional<AcquireOptions> dispatched;
        uint64_t touched{};
    };
    struct Prior
    {
        uint64_t writer_id{};
        uint64_t incarnation{};
        bool release_attempted{};
        std::optional<AcquisitionTerminationCause> cause;
        std::weak_ptr<Lease> writer;
        uint64_t touched{};
    };
    static constexpr size_t max_ids = 1024;
    static constexpr size_t max_priors = 1024;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, Id> ids;
    std::map<std::pair<StoryId, std::string>, Prior> priors;
    uint64_t clock{};
    // Callers hold mutex.
    std::string mint();
    void bound();
    void terminal(const RenewAcquisition&, AcquisitionTerminationCause);
    void releaseAttempted(StoryId, const std::string& identity, uint64_t incarnation);
    void released(StoryId, const std::string& identity, uint64_t incarnation);
};
absl::StatusOr<bool> release(State&, const RenewAcquisition&, TimePoint end);
absl::StatusOr<v1::RenewAcquisitionsResponse>
renew(State&, const std::vector<RenewAcquisition>&, grpc::ClientContext& context);
std::optional<AcquisitionTerminationCause> causeOf(const v1::RenewAcquisitionResult&);
// One bounded renewal thread per Client for every live Writer, busy or idle (RFC-G section 7).
class Scheduler
{
public:
    explicit Scheduler(std::shared_ptr<State>);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    void add(std::weak_ptr<Lease>);

private:
    void run();
    std::shared_ptr<State> state_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::weak_ptr<Lease>> leases_;
    bool stop_{};
    grpc::ClientContext* call_{};
    std::thread thread_;
};
} // namespace chronolog::client::detail
