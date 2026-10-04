#pragma once

#include "grapher/ArchiveService.h"
#include <grpcpp/grpcpp.h>
#include <functional>
#include <memory>
#include <map>
#include <thread>

namespace chronolog::grapher
{
// Follows WatchRoutes and reconciles absent archive stories at the snapshot marker (W10.17, I13.11).
class TombstoneWatcher
{
public:
    // True when the Catalog reports the story destroyed; an error means the answer is not known yet.
    using TombstoneLookup = std::function<absl::StatusOr<bool>(StoryId)>;

    TombstoneWatcher(ArchiveService& archive,
                     std::shared_ptr<grpc::Channel> channel,
                     std::string process_id,
                     std::string instance,
                     TombstoneLookup lookup);
    ~TombstoneWatcher();

private:
    void watch(std::stop_token stop);
    bool session(std::stop_token stop);
    void reconcile(std::stop_token stop);
    void conclude(uint64_t revision, uint64_t floor);

    ArchiveService& archive_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const std::string process_id_;
    const std::string instance_;
    const TombstoneLookup lookup_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::set<StoryId> seen_, pending_, tombstoned_;
    std::map<StoryId, uint64_t> learned_;
    std::map<StoryId, Epoch> epochs_;
    uint64_t revision_{}, connection_{}, marked_{}, reconciled_{};
    std::jthread watcher_, reconciler_;
};
} // namespace chronolog::grapher
