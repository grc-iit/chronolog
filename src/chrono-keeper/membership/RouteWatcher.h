#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "membership/ConfigMembership.h"
#include "membership/Watcher.h"
#include "journal/RamJournal.h"

namespace chronolog::keeper
{

// Holds Cluster.WatchRoutes open and replaces each story's route in the membership as the
// Visor sends it. The Visor sends one full snapshot on every connect. A tombstoned update drops the
// story in the journal (W10.17). After each connect a background reconciler asks the Catalog about every story
// the journal holds that the snapshot did not list, and treats a tombstoned answer as a received tombstone.
class RouteWatcher
{
public:
    // True when the Catalog reports the story destroyed; an error means the answer is not known yet.
    using TombstoneLookup = std::function<absl::StatusOr<bool>(StoryId)>;

    RouteWatcher(ConfigMembership& membership,
                 std::shared_ptr<grpc::Channel> channel,
                 std::string process_id,
                 std::string instance,
                 RamJournal* journal = nullptr,
                 TombstoneLookup lookup = nullptr,
                 std::chrono::milliseconds settle = std::chrono::milliseconds(500));
    ~RouteWatcher();

private:
    bool session(std::stop_token stop);
    void reconcile(std::stop_token stop);

    RamJournal* journal_;
    uint64_t applied_revision_{};
    std::map<StoryId, std::pair<uint64_t, Epoch>> applied_;
    std::set<StoryId> tombstoned_;
    ConfigMembership& membership_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const std::string process_id_;
    const std::string instance_;
    const TombstoneLookup lookup_;
    const std::chrono::milliseconds settle_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::set<StoryId> seen_;
    uint64_t connection_{}, reconciled_{};
    std::unique_ptr<Watcher> watcher_;
    std::jthread reconciler_;
};

} // namespace chronolog::keeper
