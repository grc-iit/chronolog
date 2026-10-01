#pragma once

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>

#include "chronolog/clock.h"
#include "chronolog/journal.h"
#include "chronolog/membership.h"

namespace chronolog
{

struct RamJournalConfig
{
    size_t payload_max_bytes{1048576};
    int64_t causal_floor_skew_limit_ns{60'000'000'000};
    // Results kept per writer for idempotent retries.
    size_t dedupe_window{65536};
};

// ACCEPTED-only Journal. DURABLE and UNSPECIFIED are rejected with UNIMPLEMENTED until the WAL lands.
class RamJournal final: public Journal
{
public:
    RamJournal(std::shared_ptr<Clock> clock,
               std::shared_ptr<const Membership> membership,
               RamJournalConfig config = {});

    absl::StatusOr<std::vector<AppendResult>> append(const AppendBatch& batch,
                                                     Durability durability = Durability::Unspecified) override;
    absl::StatusOr<std::vector<Event>> read(StoryId id, Range range) const override;
    absl::StatusOr<std::vector<Frontier>> frontier(StoryId id) const override;
    absl::StatusOr<Hlc> keeperFrontier(StoryId id) const override;

    // Writer admission, fed by the acquisition stream (or a test). A higher incarnation supersedes the
    // current one; a lower one is rejected. Re-registering the current incarnation reassigns it here.
    absl::Status registerWriter(StoryId story, uint64_t writer_id, uint64_t incarnation);
    // The writer's Keeper is no longer this one.
    void unassignWriter(StoryId story, uint64_t writer_id);
    // Fence one incarnation. Releasing an older incarnation never fences a newer one.
    void releaseWriter(StoryId story, uint64_t writer_id, uint64_t incarnation);

private:
    struct Writer
    {
        uint64_t writer_id{};
        uint64_t incarnation{};
        std::mutex mu;
        uint64_t next_sequence{1};
        Hlc last_hlc;
        bool released{};
        // HLCs of the most recent accepted sequences; back() is next_sequence - 1.
        std::deque<Hlc> window;
        // Sorted by hlc because assignment and insertion are atomic under mu.
        std::vector<Event> events;
    };

    struct Slot
    {
        uint64_t incarnation{};
        bool assigned{true};
        std::shared_ptr<Writer> current;
    };

    struct Story
    {
        std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<Writer>> writers;
        std::map<uint64_t, Slot> slots;
    };

    struct Shard
    {
        mutable std::shared_mutex mu;
        std::unordered_map<StoryId, Story> stories;
    };

    static constexpr size_t kShards = 16;

    Shard& shard(StoryId id) const { return shards_[id % kShards]; }
    absl::Status requireStory(StoryId id) const;
    std::optional<Route> currentRoute(StoryId id) const;
    AppendResult appendOne(StoryId story,
                           const AppendItem& item,
                           Durability durability,
                           int64_t now_ns,
                           std::set<std::pair<uint64_t, uint64_t>>& poisoned);
    // Ticks F first, then waits out every in-flight assignment by taking each writer lock in turn.
    Hlc seal(StoryId id, std::vector<std::shared_ptr<Writer>>& live) const;

    std::shared_ptr<Clock> clock_;
    std::shared_ptr<const Membership> membership_;
    RamJournalConfig config_;
    mutable std::array<Shard, kShards> shards_;
};

} // namespace chronolog
