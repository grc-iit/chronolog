#ifndef CHRONOLOG_KEEPER_WAL_H
#define CHRONOLOG_KEEPER_WAL_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <chronolog_types.h>
#include <ChronoKeeperConfiguration.h>
#include <WalFileSink.h>
#include <WalFormat.h>
#include <WalObserver.h>
#include <WalRecoveredState.h>
#include <WalReleaseTracker.h>
#include <WalSegmentManager.h>

namespace chronolog
{

// Whether the WAL takes a new durable write now.
enum class WalAdmit
{
    Ok,
    Unavailable, // not started, stopped, or failed after a write or sync error
    Full         // the WAL's file system is below its free-space reserve
};

struct WalStats
{
    uint64_t groups = 0;               // groups synced
    uint64_t events = 0;               // E records synced
    uint64_t bytes = 0;                // bytes of every record synced, N and D included
    uint64_t max_events_per_group = 0; // most E records in one synced group
    uint64_t pending_events = 0;       // durable events not yet released
    uint64_t pending_bytes = 0;        // frame bytes of their copies
    uint64_t segments = 0;             // segment files, the active one included
    uint64_t queued_events = 0;        // durable writes waiting for their group to sync
};

// What the keeper's durable write path needs from the WAL; W14 fakes it in
// its tests.
class DurableLog
{
public:
    virtual ~DurableLog() = default;
    virtual WalAdmit admit() const = 0;
    virtual bool isDurable(StoryId, EventSequence const&) const = 0;
    // The first check of a durable request, before admit() and the cap, so a
    // retry is answered by its first attempt and never refused while that
    // attempt is on disk or on its way there. CL_SUCCESS when the event's
    // group synced and the event is still pending. When an attempt with the
    // event's identity is queued or in its group's write or sync, waits for it
    // (a ULT yields on its tl::eventual) and returns its result: CL_SUCCESS or
    // CL_ERR_DURABILITY_UNAVAILABLE. std::nullopt when the WAL holds neither;
    // only then may the request be refused or written.
    virtual std::optional<int> lookupDurable(StoryId, EventSequence const&) = 0;
    virtual int appendDurable(ChronicleName const&, StoryName const&, LogEvent const&) = 0;
};

// The keeper's write-ahead log for durable writes: a group-commit committer on
// its own Argobots stream writes every queued record with one writev and one
// fdatasync, and only then answers the requests of that group. The WAL keeps
// each event's record until the retention store tells it (as its WalObserver)
// that the grapher confirmed the event's chunk, and deletes a segment, oldest
// first, once nothing in it is pending.
//
// Threading. A request waits on a tl::eventual, so an RPC handler yields its
// stream instead of blocking it. Every structure is guarded by one std::mutex
// that is held only for queue and table updates: never across a write, a sync,
// a wait or a log call, and never while calling out. The observer calls come
// with the retention store's tailMutex held, so the lock order is tailMutex
// first, then this mutex; the WAL never calls into the retention store or the
// ingestion queue. The committer waits for work on a std::condition_variable,
// which blocks only its own stream.
//
// Failure latch. The first write or sync error fails the whole group with
// CL_ERR_DURABILITY_UNAVAILABLE, registers none of it as pending, and leaves
// the WAL failed until the keeper restarts: after a failed fdatasync Linux may
// have dropped the dirty pages, so a later sync proves nothing.
class KeeperWal
    : public WalObserver
    , public DurableLog
{
public:
    // idle_rotation_interval: how long the active segment must have been in use
    // before the committer replaces it because every event in it was released,
    // so it can be deleted. Tests shorten it.
    KeeperWal(WalConf const&,
              std::string keeper_endpoint,
              std::unique_ptr<WalFileSink>,
              std::chrono::milliseconds idle_rotation_interval = std::chrono::minutes(1));
    ~KeeperWal() override;

    KeeperWal(KeeperWal const&) = delete;
    KeeperWal& operator=(KeeperWal const&) = delete;

    // The sink and the segment manager this WAL writes through. Recovery reads
    // the directory through them before start, so the lock it takes and the
    // segment list it finds are the ones the committer uses.
    WalFileSink& fileSink() { return *sink; }
    WalSegmentManager& segmentManager() { return segmentFiles; }

    // Opens the directory and the segment manager if recovery has not; adds
    // every recovered copy to the pending table under id_of(story), so
    // confirmations of the replayed chunks release them; takes
    // recovered.reusable_spare as the active segment when present, otherwise a
    // new spare; then starts the preparer and the committer. recovered may be
    // empty; id_of may be empty, which keeps each story's wal_story_id.
    // Returns CL_SUCCESS, or CL_ERR_UNKNOWN with the reason logged.
    int start(RecoveredState const& recovered, std::function<StoryId(RecoveredStory const&)> id_of);
    // Refuses new requests, commits what is queued, answers every waiter, then
    // joins the committer and the preparer. Safe to call more than once.
    void stop();

    // Unavailable when not started, stopped or failed; Full below the reserve.
    WalAdmit admit() const override;
    // The event's group synced and the event is still pending.
    bool isDurable(StoryId, EventSequence const&) const override;
    // See DurableLog. Answers the same way whether the WAL is running, failed
    // or stopped: a durable event stays durable, and an attempt that was
    // queued gets its answer from the committer or from stop().
    std::optional<int> lookupDurable(StoryId, EventSequence const&) override;
    // Blocks the calling thread (a ULT yields) on a tl::eventual until the
    // event's group synced, and returns CL_SUCCESS or
    // CL_ERR_DURABILITY_UNAVAILABLE. The identity is looked up first: a request
    // whose identity is already queued waits for that request's result instead
    // of queuing a copy, and one whose identity is already durable and pending
    // gets CL_SUCCESS at once; only then is a failed or stopped WAL refused.
    // CL_ERR_INVALID_ARG for an event too large for a WAL record. The event's
    // record is written from the caller's LogEvent, not copied.
    int appendDurable(ChronicleName const&, StoryName const&, LogEvent const&) override;

    WalStats stats() const;
    std::string directory() const { return walDirectory; }

    // WalObserver: never block. A D record is queued, not waited for; the
    // story's copies are dropped once it syncs.
    void chunkWritten(StoryId, StoryChunk const&) override;
    void chunkDiscarded(StoryId, StoryChunk const&) override;
    void eventDiscarded(StoryId, EventSequence const&) override;
    void storyDropped(StoryId) override;

private:
    // One answer shared by a request and every retry that joined it, so it
    // outlives whichever of them returns first.
    struct Completion
    {
        thallium::eventual<int> result;
    };

    using Identity = std::pair<StoryId, EventSequence>;

    // A queued E or D record. An E's record points into the waiting caller's
    // LogEvent, which stays alive until the completion is set. Its story's N
    // frame is encoded with it, by the caller, and written only when the
    // active segment has none for the story yet.
    struct Queued
    {
        bool dropped = false;
        StoryId story_id = 0;
        EventSequence identity{};
        wal::EventFrame event{};
        std::string names_frame;
        std::string dropped_frame;
        std::shared_ptr<Completion> completion; // empty for a D
        std::chrono::steady_clock::time_point queued_at;

        uint64_t bytes() const { return dropped ? dropped_frame.size() : event.size(); }
    };

    enum class State
    {
        Created,
        Running,
        Stopping,
        Stopped
    };

    // Called with the mutex held. True when an attempt with the identity is
    // queued or in the group being committed (completion is set to it), or
    // the identity is durable and pending (status is set to CL_SUCCESS).
    bool findIdentity(Identity const&, std::shared_ptr<Completion>& completion, int& status) const;

    void committerLoop();
    // Writes the group into the active segment (after a rotation if it does
    // not fit), syncs it and answers it.
    void commitGroup(std::vector<Queued>& group);
    // Frame bytes of the group with an N record before the first E of each
    // story not in named.
    uint64_t groupBytes(std::vector<Queued> const& group, std::unordered_set<StoryId> const& named) const;
    // Syncs the active segment and switches to a spare of at least
    // min_segment_bytes. 0 or an errno value; what names the failed step.
    int rotate(uint64_t min_segment_bytes, std::string& what);
    // Registers a synced group (or drops a failed one) and sets every
    // completion in it.
    void finishGroup(std::vector<Queued>& group, int status, uint64_t segment, uint64_t bytes);
    // Latches the failure, logs it and fails the group.
    void failGroup(std::vector<Queued>& group, int err, std::string const& what);
    // Deletes the oldest segments with nothing pending, never the active one.
    void deleteReleasedSegments();
    // Every 100 ms: the free-space sample, the idle rotation and the max_bytes
    // warning.
    void periodicWork();
    void sampleFreeSpace();

    std::string const walDirectory;
    uint64_t const segmentBytes;
    uint64_t const maxBytes;
    uint64_t const groupCommitMaxBytes;
    uint32_t const groupCommitWindowUs;
    uint64_t const reserveMinBytes;
    uint32_t const reservePercent;
    std::string const keeperEndpoint;
    std::chrono::milliseconds const idleRotationInterval;
    uint64_t const headerBytes; // size of a segment's H record

    std::unique_ptr<WalFileSink> sink;
    WalSegmentManager segmentFiles; // uses *sink, so declared after it

    // admit() reads these without the mutex
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> belowReserve{false};

    // guarded by mutex
    mutable std::mutex mutex;
    std::condition_variable workReady;
    State state = State::Created;
    std::deque<Queued> queue;
    uint64_t queuedBytes = 0;
    // every queued E, and the E records of the group being committed
    std::map<Identity, std::shared_ptr<Completion>> inFlight;
    WalReleaseTracker tracker;
    WalStats counters;

    // the committer's own, also used by start and stop before and after it runs
    int activeFd = -1;
    uint64_t activeSegment = 0;
    uint64_t activeSize = 0;
    uint64_t activeDataStart = 0;
    uint64_t writeOffset = 0;
    std::chrono::steady_clock::time_point activeSince;
    std::unordered_set<StoryId> namedInActive; // stories with an N record in the active segment
    std::unordered_map<uint64_t, uint64_t> segmentSizes;
    std::chrono::steady_clock::time_point nextPeriodicWork;
    std::chrono::steady_clock::time_point lastSizeWarning;
    std::chrono::steady_clock::time_point lastFreeSpaceWarning;
    std::chrono::steady_clock::time_point lastDeleteWarning;

    thallium::managed<thallium::xstream> committerStream;
    thallium::managed<thallium::thread> committerThread;
};

} // namespace chronolog

#endif // CHRONOLOG_KEEPER_WAL_H
