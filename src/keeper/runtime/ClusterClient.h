#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "chronolog/clock.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "common/clock/ClockAudit.h"
#include "keeper/journal/RamJournal.h"
#include "keeper/membership/AcquisitionWatcher.h"
#include "keeper/membership/ConfigMembership.h"
#include "common/rpc/Channel.h"

namespace chronolog::keeper
{

// Registers this Keeper with the Visor and heartbeats it. A heartbeat carries the
// watcher's applied_revision, which is how the Visor learns a release fence took effect.
class ClusterClient
{
public:
    struct Options
    {
        std::string process_id;
        std::string instance;
        std::string endpoint;
        std::chrono::milliseconds interval{5000};
        std::string recovered_instance{};
        // Bootstrap liveness timers, in force until a Register reply carries the Visor's own (A9). The deadline
        // of Register and Heartbeat derives from the timers in force (M11.3).
        uint32_t keeper_failure_timeout_ms{15000};
        uint32_t release_fence_timeout_ms{2000};
        // Physical clock that brackets each Register and Heartbeat attempt for the clock audit; none disables it.
        std::shared_ptr<Clock> clock{};
        // Monotonic nanoseconds; steady_clock when empty.
        std::function<int64_t()> monotonic{};
        // Assertions of the Catalog S and D, never independent admission policy.
        int64_t causal_floor_skew_limit_ns{PhysicalPolicy{}.skew_limit_ns};
        uint32_t reserve_ahead_ms{1000};
    };

    ClusterClient(std::shared_ptr<grpc::Channel> channel,
                  Options options,
                  RamJournal& journal,
                  ConfigMembership& membership,
                  const AcquisitionWatcher& acquisitions);
    ~ClusterClient();

    absl::Status registerNow();
    absl::Status heartbeatNow();
    absl::Status extendNow();

    // Starts the loop: register with backoff, then heartbeat every interval or when kicked.
    void start();
    // Heartbeat now instead of waiting for the interval.
    void kick();

    bool registered() const { return registered_; }
    std::chrono::milliseconds heartbeatDeadline() const { return std::chrono::milliseconds(deadline_ms_.load()); }
    // Per-Visor-replica clock audit state; observational only (B45 Part 1).
    std::vector<ClockAuditEntry> clockAudit() const;

private:
    void loop(std::stop_token stop);
    void applyRoutes(const google::protobuf::RepeatedPtrField<internal::v1::RouteUpdate>& routes);
    absl::Status adoptTimeouts(const internal::v1::MembershipPolicy& policy);
    struct ClockMark
    {
        TimeReading physical;
        int64_t monotonic_ns{};
    };
    ClockMark mark() const;
    int64_t monotonicNow() const;
    // Feeds one attempt's bracket and reply to the audit and logs its transition.
    template <class Response>
    void audit(const ClockMark& start, const grpc::Status& rpc, const Response& response)
    {
        if(!options_.clock)
            return;
        const auto end = mark();
        ClockAuditSample sample;
        sample.t0 = start.physical;
        sample.t1 = end.physical;
        sample.m0_ns = start.monotonic_ns;
        sample.m1_ns = end.monotonic_ns;
        sample.replied = rpc.ok();
        if(response.has_physical())
            sample.visor = physicalReading(response.physical());
        sample.responder = {response.clock_responder().replica_id(), response.clock_responder().instance()};
        audited(sample);
    }
    static TimeReading physicalReading(const v1::TimeReading& reading);
    void audited(const ClockAuditSample& sample);
    void logTransition(const ClockAuditTransition& transition) const;

    // One deadline for the whole call, so failing over never outlasts the timer the call feeds. Each attempt
    // may use all that is left: only an UNAVAILABLE answer moves on to a replica, and an attempt that runs out
    // of time ends the call, so a budget held back for the replicas would only starve the attempt doing the work
    // (a follower forwarding to a loaded leader).
    template <class Call>
    grpc::Status invoke(Call call, std::chrono::milliseconds deadline)
    {
        std::lock_guard lock(rpc_mutex_);
        const auto end = std::chrono::system_clock::now() + deadline;
        auto attempt = [&](auto& stub)
        {
            grpc::ClientContext context;
            rpc::withDeadline(context, end);
            return call(stub, context);
        };
        auto status = attempt(*stub_);
        if(status.error_code() != grpc::StatusCode::UNAVAILABLE)
            return status;
        for(auto& replica: replicas_)
        {
            status = attempt(*replica);
            if(status.ok())
            {
                stub_.swap(replica);
                return status;
            }
            if(status.error_code() != grpc::StatusCode::UNAVAILABLE)
                return status;
        }
        return status;
    }
    std::mutex rpc_mutex_;
    std::vector<std::unique_ptr<internal::v1::Cluster::Stub>> replicas_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const Options options_;
    RamJournal& journal_;
    ConfigMembership& membership_;
    const AcquisitionWatcher& acquisitions_;
    std::atomic<std::chrono::milliseconds::rep> extend_deadline_ms_{0};
    std::atomic<uint32_t> failure_timeout_ms_;
    std::atomic<uint32_t> fence_timeout_ms_;
    std::atomic<std::chrono::milliseconds::rep> deadline_ms_{0};
    ClockAudit audit_;
    std::vector<std::string> visor_endpoints_;
    std::atomic<bool> registered_{false};
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool kicked_{};
    std::jthread thread_;
};

} // namespace chronolog::keeper
