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
    // The first check of a durable request, before admit() and the cap, so a
    // retry is answered by its first attempt and never refused while that
    // attempt is on disk or on its way there. CL_SUCCESS when the event's
    // group synced and the event is still pending. When an attempt with the
    // event's identity is queued or in its group's write or sync, waits for it
    // (a ULT yields on its tl::eventual) and returns its result: CL_SUCCESS,
    // CL_ERR_DURABILITY_UNAVAILABLE, or CL_ERR_DURABILITY_UNKNOWN when its
    // group failed and could not be erased. std::nullopt when the WAL holds
    // neither; only then may the request be refused or written.
    virtual std::optional<int> lookupDurable(StoryId, EventSequence const&) = 0;
    // Sets wrote when this call queued the event's record itself, so the
    // caller ingests the event only then. It stays false when the call was
    // answered by an earlier attempt with the same identity, which ingested
    // the event when its own group synced.
    virtual int appendDurable(ChronicleName const&, StoryName const&, LogEvent const&, bool& wrote) = 0;
};

// The keeper's write-ahead log for durable writes: a group-commit committer on
// its own Argobots stream writes every queued record with one writev and one
// fdatasync, and only then answers the requests of that group. The WAL keeps
// each event's record until the retention store tells it (as its WalObserver)
// that the grapher confirmed the event's chunk, and deletes a segment, oldest
// first, once nothing in it is pending.
//
// Threading. A request waits on a tl::eventual, so an RPC handler yields its
// stream instead of blocking it. Every structure is guarded by one mutex (a
// std::mutex that counts its waiters) that is held only for queue and table
// updates: never across a write, a sync, a wait or a log call, and never while
// calling out. Work that grows with a group, a chunk or a story's backlog
// (registering a group, releasing a chunk's events, dropping a story at its D
// record) is done a bounded batch per hold, and a waiting thread gets the
// mutex between batches, so a waiting RPC stream is not held up for long.
// The observer calls come with the retention store's tailMutex held, so the
// lock order is tailMutex first, then this mutex; the WAL never calls into the
// retention store or the ingestion queue. The committer waits for work on a
// std::condition_variable_any, which blocks only its own stream.
//
// Failure latch. The first write or sync error fails the whole group,
// registers none of it as pending, and leaves the WAL failed until the keeper
// restarts: after a failed fdatasync Linux may mark the dirty pages clean and
// keep them, so a later sync proves nothing. Before any record of the group
// is answered, the bytes its write started on are overwritten with zeros and
// synced, so a keeper restarted on the same boot does not read the group back
// as durable; the group then gets CL_ERR_DURABILITY_UNAVAILABLE. When the
// zeros cannot be written or synced, the group may be on disk and gets
// CL_ERR_DURABILITY_UNKNOWN. A rotation that fails before the group is written
// has nothing to erase. A failed sync of the active segment before an idle
// rotation latches the same way; when only the spare for that rotation cannot
// be had, the committer keeps writing to the active segment and tries again
// an interval later.
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
    // Returns CL_SUCCESS, or CL_ERR_UNKNOWN with the reason logged. A failed
    // start leaves the WAL not started, with no recovered copy pending and
    // every request made meanwhile answered CL_ERR_DURABILITY_UNAVAILABLE;
    // the directory and the segment manager stay open, and start may be called
    // again. Once started, or stopped, it refuses.
    int start(RecoveredState const& recovered, std::function<StoryId(RecoveredStory const&)> id_of);
    // Refuses new requests, commits what is queued, answers every waiter, then
    // joins the committer and the preparer. Safe to call more than once.
    void stop();

    // Unavailable when not started, stopped or failed; Full below the reserve.
    WalAdmit admit() const override;
    // The event's group synced and the event is still pending. Not part of
    // DurableLog: the write path asks lookupDurable, which also covers an
    // attempt still on its way; this is for tests and diagnostics.
    bool isDurable(StoryId, EventSequence const&) const;
    // See DurableLog. Answers the same way whether the WAL is running, failed
    // or stopped: a durable event stays durable, and an attempt that was
    // queued gets its answer from the committer or from stop().
    std::optional<int> lookupDurable(StoryId, EventSequence const&) override;
    // Blocks the calling thread (a ULT yields) on a tl::eventual until the
    // event's group synced, and returns CL_SUCCESS or
    // CL_ERR_DURABILITY_UNAVAILABLE; CL_ERR_DURABILITY_UNKNOWN when the
    // group's write or sync failed and its bytes could not be erased (see the
    // failure latch above). The identity is looked up first: a request
    // whose identity is already queued waits for that request's result instead
    // of queuing a copy, and one whose identity is already durable and pending
    // gets CL_SUCCESS at once; only then is a failed or stopped WAL refused.
    // CL_ERR_INVALID_ARG for an event too large for a WAL record. The event's
    // record is written from the caller's LogEvent, not copied. wrote is set
    // only when this call queued the record (see DurableLog).
    int appendDurable(ChronicleName const&, StoryName const&, LogEvent const&, bool& wrote) override;

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
    // Undoes a start whose committer could not be created: back to Created,
    // with nothing pending and every request queued meanwhile refused.
    void abandonStart(std::string const& reason);

    // Called with the mutex held, after the tracker's addCopy.
    void noteAdded(StoryId, EventSequence const&);
    // Called with the mutex held. Pops released identities off the front of
    // the story's add order, at most budget of them, and forgets the add
    // order once the story has nothing pending.
    void pruneAddOrder(StoryId, std::size_t budget);
    // releaseChunk for chunkWritten and chunkDiscarded, a batch per hold when
    // both the chunk and the story's pending events are large.
    void releaseChunkEvents(StoryId, StoryChunk const&);
    // The committer, at a D record that synced: drops every copy of the story
    // added before it, a batch per hold.
    void dropStoryCopies(StoryId);

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
    // The switch alone: takes a spare of at least min_segment_bytes and makes
    // it the active segment. The active segment is unchanged on failure.
    int switchToSpare(uint64_t min_segment_bytes, std::string& what);
    // Registers a synced group (or drops a failed one) and sets every
    // completion in it.
    void finishGroup(std::vector<Queued>& group, int status, uint64_t segment, uint64_t bytes);
    // A latched WAL writes no D record, so the destroys in a latched group go
    // to the unlogged-drops file for recovery, and their stories' copies are
    // dropped here (committer thread only).
    void recordUnloggedDrops(std::vector<Queued> const& group);
    // Latches the failure, logs it and fails the group. written is the length
    // of the range from writeOffset of the active segment that the group's
    // write started on, 0 when it failed before the write. Before any answer
    // that range is erased: CL_ERR_DURABILITY_UNAVAILABLE when nothing was
    // written or the erase succeeded, CL_ERR_DURABILITY_UNKNOWN when it failed
    // and the group may be on disk.
    void failGroup(std::vector<Queued>& group, int err, std::string const& what, uint64_t written);
    // Overwrites [offset, offset + bytes) of the active segment with zeros
    // and data-syncs it. 0 or an errno value; what names the failed step.
    int eraseRange(uint64_t offset, uint64_t bytes, std::string& what);
    // Deletes the oldest segments with nothing pending, never the active one.
    // After a removal whose directory sync failed, deletes nothing newer until
    // a directory sync succeeds: a segment that came back after a crash must
    // still find the D records of the newer segments.
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

    // The WAL's mutex: a std::mutex that counts the threads waiting for it.
    // glibc's mutex is not fair: a loop that unlocks and locks again at once
    // nearly always wins over the thread its unlock woke, so batches alone
    // would not let anyone in. Between batches such a loop calls
    // letWaitersIn, which gives a waiting thread a moment to take it first.
    class WalMutex
    {
    public:
        void lock()
        {
            ++waiting;
            inner.lock();
            --waiting;
            ++acquired;
        }
        void unlock() { inner.unlock(); }
        // Called without the mutex. While a thread waits for it, yields (the
        // OS thread, never to Argobots: a caller may hold the retention
        // store's tailMutex) until one has taken it, for at most a moment.
        void letWaitersIn();

    private:
        std::mutex inner;
        std::atomic<uint32_t> waiting{0};
        std::atomic<uint64_t> acquired{0};
    };

    // admit() reads these without the mutex
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> belowReserve{false};

    // guarded by mutex
    mutable WalMutex mutex;
    std::condition_variable_any workReady;
    State state = State::Created;
    std::deque<Queued> queue;
    uint64_t queuedBytes = 0;
    // every queued E, and the E records of the group being committed
    std::map<Identity, std::shared_ptr<Completion>> inFlight;
    WalReleaseTracker tracker;
    // Per story with pending copies, the identities added to the tracker, in
    // log order, released ones pruned from the front. Only lets a D drop a
    // large backlog a batch at a time: the tracker can only drop a whole story
    // at once, and it still does, after the batches, for anything missed here.
    std::unordered_map<StoryId, std::deque<EventSequence>> addOrder;
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
    bool directorySyncOwed = false; // a segment was removed, but the directory sync after it failed

    thallium::managed<thallium::xstream> committerStream;
    thallium::managed<thallium::thread> committerThread;
};

} // namespace chronolog

#endif // CHRONOLOG_KEEPER_WAL_H
