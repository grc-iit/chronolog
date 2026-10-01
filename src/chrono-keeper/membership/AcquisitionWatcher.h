#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "journal/RamJournal.h"
#include "membership/Watcher.h"

namespace chronolog::keeper
{

// Holds Cluster.WatchAcquisitions open, applies the full snapshot and then each delta to
// the journal's writer admission, and reconnects with backoff. A snapshot also fences
// every local writer it no longer lists, because a RELEASED delta can be missed while
// disconnected. applied_revision is what the Keeper reports in heartbeats.
class AcquisitionWatcher
{
public:
    // Called after a RELEASED update or a fencing snapshot was applied, so the owner can
    // heartbeat immediately instead of waiting for the interval.
    using FenceHook = std::function<void()>;

    AcquisitionWatcher(RamJournal& journal, std::string keeper_id, FenceHook on_fence = nullptr);
    ~AcquisitionWatcher();

    // Starts the background stream against the Visor's Cluster service.
    void start(std::shared_ptr<grpc::Channel> channel);

    void applySnapshot(const internal::v1::AcquisitionSnapshot& snapshot);
    void applyUpdate(const internal::v1::AcquisitionUpdate& update);

    uint64_t appliedRevision() const;
    bool waitApplied(uint64_t revision, std::chrono::milliseconds timeout) const;

private:
    bool session(std::stop_token stop);
    void advance(uint64_t revision);

    RamJournal& journal_;
    const std::string keeper_id_;
    const FenceHook on_fence_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    uint64_t applied_{};
    std::unique_ptr<Watcher> watcher_;
};

} // namespace chronolog::keeper
