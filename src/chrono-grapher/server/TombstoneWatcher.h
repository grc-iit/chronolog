#pragma once

#include "chrono-grapher/server/ArchiveService.h"
#include <grpcpp/grpcpp.h>
#include <functional>
#include <memory>
#include <thread>

namespace chronolog::grapher
{
// Holds Cluster.WatchRoutes open and acts only on tombstoned updates (W10.17, I13.11). The Visor sends a full
// snapshot on every connect and tombstones never appear in one, so after each connect a background reconciler asks the
// Catalog about every story the archive holds that the snapshot did not list, and treats a tombstoned answer as a
// received tombstone. Reconciliation never gates serving: until the Catalog answers, chunks are accepted and a later
// tombstone removes them.
class TombstoneWatcher
{
public:
    // True when the Catalog reports the story destroyed; an error means the answer is not known yet.
    using TombstoneLookup = std::function<absl::StatusOr<bool>(StoryId)>;

    TombstoneWatcher(ArchiveService& archive,
                     std::shared_ptr<grpc::Channel> channel,
                     std::string process_id,
                     std::string instance,
                     TombstoneLookup lookup,
                     std::chrono::milliseconds settle = std::chrono::milliseconds(500));
    ~TombstoneWatcher();

private:
    void watch(std::stop_token stop);
    bool session(std::stop_token stop);
    void reconcile(std::stop_token stop);

    ArchiveService& archive_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const std::string process_id_;
    const std::string instance_;
    const TombstoneLookup lookup_;
    const std::chrono::milliseconds settle_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::set<StoryId> seen_;
    uint64_t connection_{}, reconciled_{};
    std::jthread watcher_, reconciler_;
};
} // namespace chronolog::grapher
