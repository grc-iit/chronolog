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
// Visor sends it. The Visor sends one full snapshot at revision R on every connect, then a snapshot_end marker.
// A tombstoned update drops the story in the journal (W10.17). At the marker, a story the journal holds that this
// watcher learned from a route at a revision at or below R and the snapshot did not list was destroyed, and is
// dropped as a received tombstone. A background reconciler asks the Catalog about every other unlisted story.
// A stream that ends before its marker reconciles nothing. The journal acknowledges R at the marker and each later
// revision once it is applied.
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
                 TombstoneLookup lookup = nullptr);
    ~RouteWatcher();

private:
    bool session(std::stop_token stop);
    void apply(const internal::v1::WatchRoutesResponse& message, bool acknowledge);
    void conclude(uint64_t revision, uint64_t floor);
    void reconcile(std::stop_token stop);

    RamJournal* journal_;
    uint64_t applied_revision_{};
    std::map<StoryId, std::pair<uint64_t, Epoch>> applied_;
    std::set<StoryId> tombstoned_;
    // Lowest route revision this watcher applied for each story.
    std::map<StoryId, uint64_t> learned_;
    ConfigMembership& membership_;
    std::unique_ptr<internal::v1::Cluster::Stub> stub_;
    const std::string process_id_;
    const std::string instance_;
    const TombstoneLookup lookup_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::set<StoryId> seen_;
    std::set<StoryId> pending_;
    uint64_t connection_{}, marked_{}, reconciled_{};
    std::unique_ptr<Watcher> watcher_;
    std::jthread reconciler_;
};

} // namespace chronolog::keeper
