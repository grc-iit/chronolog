#pragma once

#include <array>
#include <condition_variable>
#include "clock/CeilingControl.h"
#include <atomic>
#include <functional>
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
    std::string process_id;
    std::string instance;
    uint32_t append_ceiling_wait_ms{1000};
    size_t payload_max_bytes{1048576};
    int64_t causal_floor_skew_limit_ns{60'000'000'000};
    // Results kept per writer for idempotent retries.
    size_t dedupe_window{65536};
};

// RAM storage with an optional asynchronous persistence implementation.
class RamJournal: public Journal
{
public:
    RamJournal(std::shared_ptr<Clock> clock,
               std::shared_ptr<const Membership> membership,
               RamJournalConfig config = {});

    absl::StatusOr<std::vector<AppendResult>> append(const AppendBatch& batch,
                                                     Durability durability = Durability::Unspecified) override;
    using AppendCallback = std::function<void(absl::StatusOr<std::vector<AppendResult>>)>;
    void appendAsync(const AppendBatch& batch, Durability durability, AppendCallback done);
    void enableDynamic(std::string instance,
                       Hlc restart_floor = {},
                       int64_t physical_floor = 0,
                       int64_t acceptance_budget = 3'000'000'000,
                       int64_t hlc_budget = 30'000'000'000);
    void extendCeiling(Hlc ceiling, int64_t physical_ceiling);
    void
    applyRoute(StoryId story, RouteState state, bool observe_floor, uint64_t revision, std::function<void()> install);
    uint64_t appliedRouteRevision() const;
    void acknowledgeRoutes(uint64_t revision);
    std::optional<Predecessor> retiredOwner(StoryId story) const;
    std::vector<Predecessor> predecessorOwners(StoryId story) const;
    size_t ceilingWaiters() const { return ceiling_waiters_; }
    std::string instance() const;
    Hlc wantedCeiling() const;
    int64_t realtime() const;
    bool dynamic() const;
    bool retiredDrained(StoryId story) const;
    bool neverHeldEvent(StoryId story) const;
    void setAdmissionReady(bool ready) { admission_ready_.store(ready); }
    absl::StatusOr<std::vector<Event>> read(StoryId id, Range range) const override;
    absl::StatusOr<std::vector<Frontier>> frontier(StoryId id) const override;
    absl::StatusOr<Hlc> keeperFrontier(StoryId id) const override;
    absl::StatusOr<int64_t> physicalFrontier(StoryId id) const override;
    virtual bool hasPhysicalPolicy() const { return true; }

    struct SealedView
    {
        // Exclusive frontier F ticked once before any writer lock was taken.
        Hlc sealed;
        // Every live writer incarnation, all carrying F.
        std::vector<Frontier> frontiers;
    };
    // One tick for both the Keeper seal and the per-writer frontiers, so FetchHot can scan after it.
    absl::StatusOr<SealedView> sealedView(StoryId id) const;

    struct SealedRead
    {
        SealedView view;
        std::vector<Event> events;
        Hlc evicted_below;
    };
    absl::StatusOr<SealedRead> sealedRead(StoryId id,
                                          Range range,
                                          std::optional<Hlc> tick = std::nullopt,
                                          std::optional<Range> physical_filter = std::nullopt) const;
    Hlc sealTick() const { return reserveFrontier(clock_->tick()); }

    std::vector<StoryId> storyIds() const;
    void eraseEvents(StoryId story, Range range, bool advance_floor = false);
    Hlc evictionFloor(StoryId story) const;

    struct WriterKey
    {
        StoryId story_id{};
        uint64_t writer_id{};
        uint64_t incarnation{};
        auto operator<=>(const WriterKey&) const = default;
    };
    // Current, unreleased incarnation of every writer registered here, assigned or not.
    std::vector<WriterKey> liveWriters() const;

    // Writer admission, fed by the acquisition stream (or a test). A higher incarnation supersedes the
    // current one; a lower one is rejected. Re-registering the current incarnation reassigns it here.
    absl::Status registerWriter(StoryId story, uint64_t writer_id, uint64_t incarnation);
    // The writer's Keeper is no longer this one.
    void unassignWriter(StoryId story, uint64_t writer_id);
    // Fence one incarnation. Releasing an older incarnation never fences a newer one.
    void releaseWriter(StoryId story, uint64_t writer_id, uint64_t incarnation);

protected:
    virtual bool supportsDurable() const { return false; }
    virtual bool durableAvailable() const { return true; }
    virtual void finishAppend(AppendCallback done, absl::StatusOr<std::vector<AppendResult>> results)
    {
        done(std::move(results));
    }
    virtual void persist(const Event&, std::function<void(absl::Status)>);
    virtual Hlc reserveFrontier(Hlc frontier) const { return frontier; }
    virtual absl::StatusOr<int64_t> reservePhysicalFrontier(StoryId, int64_t frontier) const { return frontier; }
    virtual void writerScanned(WriterKey) const {}
    virtual void assignmentObserved(Hlc) {}
    void restore(const Event& event);
    struct WriterCheckpoint
    {
        WriterKey key;
        uint64_t next_sequence{};
        Hlc last_hlc;
        bool released{}, assigned{};
        std::vector<AppendResult> window;
    };
    std::vector<WriterCheckpoint> checkpointWriters() const;
    void restoreWriter(const WriterCheckpoint& checkpoint);


private:
    class Gate
    {
    public:
        void lock()
        {
            std::unique_lock l(mu);
            ++waiting;
            cv.wait(l, [&] { return !writer && readers == 0; });
            --waiting;
            writer = true;
        }
        void unlock()
        {
            std::lock_guard l(mu);
            writer = false;
            cv.notify_all();
        }
        void lock_shared()
        {
            std::unique_lock l(mu);
            cv.wait(l, [&] { return !writer && waiting == 0; });
            ++readers;
        }
        void unlock_shared()
        {
            std::lock_guard l(mu);
            --readers;
            cv.notify_all();
        }

    private:
        std::mutex mu;
        std::condition_variable cv;
        size_t readers{}, waiting{};
        bool writer{};
    };
    struct Admission
    {
        Gate gate;
        RouteState state;
        bool installed{}, observe{}, raise{};
        Hlc observe_floor;
        int64_t physical_floor{};
        uint64_t revision{};
    };
    std::shared_ptr<Admission> admission(StoryId story) const;
    bool scheduleSteps(Admission& admission);
    Hlc capSeal(StoryId story, Hlc seal) const;
    mutable std::mutex dynamic_mu_;
    mutable std::map<StoryId, std::shared_ptr<Admission>> admissions_;
    std::atomic<size_t> ceiling_waiters_{};
    std::condition_variable ceiling_cv_;
    uint64_t ceiling_generation_{}, applied_route_revision_{};
    bool dynamic_{};
    std::string instance_;
    Hlc ceiling_{}, restart_floor_{};
    int64_t physical_ceiling_{}, restart_physical_{}, acceptance_budget_{}, hlc_budget_{};

    struct Pending
    {
        Event event;
        std::vector<std::function<void(AppendResult)>> waiters;
    };
    struct Writer
    {
        uint64_t writer_id{};
        uint64_t incarnation{};
        std::mutex mu;
        uint64_t next_sequence{1};
        Hlc last_hlc;
        bool released{};
        // Results for recent sequences; back() is next_sequence - 1.
        std::map<uint64_t, AppendResult> window;
        std::map<uint64_t, Pending> pending;
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
        Hlc evicted_below;
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
    std::optional<AppendResult> appendOne(StoryId story,
                                          const AppendItem& item,
                                          Durability durability,
                                          int64_t now_ns,
                                          std::set<std::pair<uint64_t, uint64_t>>& poisoned,
                                          std::function<void(AppendResult)> done);
    // Ticks F first, then waits out every in-flight assignment by taking each writer lock in turn.
    Hlc seal(StoryId id,
             std::vector<std::shared_ptr<Writer>>& live,
             const Range* range = nullptr,
             std::vector<Event>* events = nullptr,
             std::optional<Hlc> tick = std::nullopt,
             std::optional<Range> physical_filter = std::nullopt) const;
    static void scan(const Writer& writer,
                     Range range,
                     std::vector<Event>& out,
                     std::optional<Range> physical_filter = std::nullopt);
    void complete(const std::shared_ptr<Writer>& writer, uint64_t sequence, absl::Status status);

    std::shared_ptr<Clock> clock_;
    std::shared_ptr<const Membership> membership_;
    RamJournalConfig config_;
    std::atomic<bool> admission_ready_{true};
    mutable std::array<Shard, kShards> shards_;
    mutable std::mutex physical_mu_;
    mutable std::map<StoryId, int64_t> physical_reports_;
};

} // namespace chronolog
