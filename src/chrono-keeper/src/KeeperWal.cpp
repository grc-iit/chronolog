#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <sys/uio.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <KeeperWal.h>
#include <StoryChunk.h>

namespace chronolog
{

namespace
{

// how often the committer samples free space and looks for segments to delete
constexpr std::chrono::milliseconds kPeriodicInterval(100);
// how often a repeated warning is logged
constexpr std::chrono::minutes kWarningInterval(1);
// The most entries one hold of the mutex handles: group entries registered,
// events released, identities pruned. Keeps each hold short whatever the size
// of the group, the chunk or the story's backlog.
constexpr std::size_t kLockBatch = 1024;
// released identities pruned from a story's add order for each one added
constexpr std::size_t kPruneOnAdd = 2;
// the longest a batch loop waits for a waiting thread to take the mutex: a
// woken thread takes it within microseconds, so this only bounds the wait
constexpr std::chrono::microseconds kHandOffLimit(500);

iovec iovOf(char const* data, std::size_t len) { return iovec{const_cast<char*>(data), len}; }

} // namespace

void KeeperWal::WalMutex::letWaitersIn()
{
    if(waiting.load() == 0)
    {
        return;
    }
    uint64_t const before = acquired.load();
    auto const deadline = std::chrono::steady_clock::now() + kHandOffLimit;
    while(waiting.load() != 0 && acquired.load() == before && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
}

KeeperWal::KeeperWal(WalConf const& conf,
                     std::string keeper_endpoint,
                     std::unique_ptr<WalFileSink> file_sink,
                     std::chrono::milliseconds idle_rotation_interval)
    : walDirectory(conf.dir)
    , segmentBytes(conf.segment_bytes)
    , maxBytes(conf.max_bytes)
    , groupCommitMaxBytes(conf.group_commit_max_bytes)
    , groupCommitWindowUs(conf.group_commit_window_us)
    , reserveMinBytes(conf.reserve_min_bytes)
    , reservePercent(conf.reserve_percent)
    , keeperEndpoint(std::move(keeper_endpoint))
    , idleRotationInterval(idle_rotation_interval)
    , headerBytes(wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, 0, 0, keeperEndpoint}).size())
    , sink(std::move(file_sink))
    , segmentFiles(*sink, conf.segment_bytes, keeperEndpoint, conf.dir)
{}

KeeperWal::~KeeperWal() { stop(); }

int KeeperWal::start(RecoveredState const& recovered, std::function<StoryId(RecoveredStory const&)> id_of)
{
    bool created = false;
    {
        std::lock_guard<WalMutex> lock(mutex);
        created = (state == State::Created);
    }
    if(!created)
    {
        LOG_ERROR("[KeeperWal] The WAL in {} was already started", walDirectory);
        return CL_ERR_UNKNOWN;
    }

    // recovery may have opened both through fileSink() and segmentManager()
    int err = sink->openDirectory(walDirectory);
    if(err != 0 && err != EBUSY)
    {
        LOG_CRITICAL("[KeeperWal] Cannot open WAL directory {}: {}", walDirectory, std::strerror(err));
        return CL_ERR_UNKNOWN;
    }
    if(!segmentFiles.isOpen() && (err = segmentFiles.open()) != 0)
    {
        LOG_CRITICAL("[KeeperWal] {}", segmentFiles.error());
        return CL_ERR_UNKNOWN;
    }

    // Every step that can fail before the committer comes before the recovered
    // copies are added, so a start that fails there can be made again.
    int fd = -1;
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    uint64_t size = 0;
    bool adopted = false;
    if(recovered.reusable_spare)
    {
        err = segmentFiles.adoptExisting(*recovered.reusable_spare, fd, data_offset, &size);
        if(err == 0)
        {
            segment = *recovered.reusable_spare;
            adopted = true;
        }
        else
        {
            LOG_WARNING("[KeeperWal] Not reusing the header-only segment: {}; starting a new one",
                        segmentFiles.error());
        }
    }
    if(!adopted && (err = segmentFiles.takeSpare(segmentBytes, segment, fd, data_offset, &size)) != 0)
    {
        LOG_CRITICAL("[KeeperWal] Cannot start the WAL in {}: {}", walDirectory, segmentFiles.error());
        return CL_ERR_UNKNOWN;
    }

    auto const now = std::chrono::steady_clock::now();
    activeFd = fd;
    activeSegment = segment;
    activeSize = size;
    activeDataStart = data_offset;
    writeOffset = data_offset;
    activeSince = now;
    namedInActive.clear();
    for(uint64_t listed: segmentFiles.segments())
    {
        segmentSizes.emplace(listed, segmentBytes); // a recovered segment's size, near enough for the warning
    }
    segmentSizes[segment] = size;
    nextPeriodicWork = now; // the first pass deletes recovered segments with nothing pending
    lastSizeWarning = now - kWarningInterval;
    lastFreeSpaceWarning = now - kWarningInterval;
    lastDeleteWarning = now - kWarningInterval;
    sampleFreeSpace();

    // Recovered copies are pending under the story's live id: the grapher
    // confirms the replayed chunks under that id.
    std::vector<std::pair<StoryId, RecoveredStory const*>> recovered_stories;
    for(auto const& [wal_story_id, story]: recovered.stories)
    {
        recovered_stories.emplace_back(id_of ? id_of(story) : wal_story_id, &story);
    }
    std::size_t recovered_events = 0;
    {
        std::lock_guard<WalMutex> lock(mutex);
        for(auto const& [story_id, story]: recovered_stories)
        {
            for(auto const& [identity, copies]: story->copies)
            {
                for(auto const& [copy_segment, bytes]: copies)
                {
                    tracker.addCopy(story_id, identity, copy_segment, bytes);
                }
                noteAdded(story_id, identity);
                ++recovered_events;
            }
        }
        counters.segments = segmentFiles.segments().size();
        state = State::Running;
    }
    segmentFiles.startPreparer();
    try
    {
        committerStream = thallium::xstream::create();
        committerThread = committerStream->make_thread([this]() { committerLoop(); });
    }
    catch(std::exception const& error)
    {
        // without a committer every request would wait forever, and stop
        // would join a thread that does not exist
        abandonStart(error.what());
        return CL_ERR_UNKNOWN;
    }
    running = true;

    LOG_INFO("[KeeperWal] Started the WAL in {}: writing segment {} ({}), {} segments, {} recovered events pending",
             walDirectory,
             wal::segmentFileName(segment),
             adopted ? "reused" : "new",
             segmentFiles.segments().size(),
             recovered_events);
    return CL_SUCCESS;
}

void KeeperWal::abandonStart(std::string const& reason)
{
    std::deque<Queued> refused;
    {
        std::lock_guard<WalMutex> lock(mutex);
        // as before start; a request queued meanwhile is refused below
        state = State::Created;
        refused.swap(queue);
        queuedBytes = 0;
        inFlight.clear();
        tracker = WalReleaseTracker();
        addOrder.clear();
        counters = WalStats();
    }
    for(Queued& entry: refused)
    {
        if(entry.completion)
        {
            entry.completion->result.set_value(CL_ERR_DURABILITY_UNAVAILABLE);
        }
    }
    if(!committerStream->is_null())
    {
        try
        {
            committerStream->join();
        }
        catch(std::exception const&)
        {}
    }
    committerThread = thallium::managed<thallium::thread>();
    committerStream = thallium::managed<thallium::xstream>();
    segmentFiles.stopPreparer();
    // the segment stays listed, header-only, and is deleted once a later start
    // has taken a newer one
    sink->closeFile(activeFd);
    activeFd = -1;
    namedInActive.clear();
    segmentSizes.clear();
    LOG_CRITICAL("[KeeperWal] Cannot start the WAL committer in {}: {}. The WAL is not started", walDirectory, reason);
}

void KeeperWal::stop()
{
    {
        std::lock_guard<WalMutex> lock(mutex);
        if(state == State::Created)
        {
            state = State::Stopped;
            return;
        }
        if(state != State::Running)
        {
            return;
        }
        // from here appendDurable refuses; the committer drains the queue first
        state = State::Stopping;
    }
    running = false;
    workReady.notify_all();
    committerThread->join();
    committerStream->join();
    committerThread = thallium::managed<thallium::thread>();
    committerStream = thallium::managed<thallium::xstream>();
    segmentFiles.stopPreparer();
    if(activeFd >= 0)
    {
        sink->closeFile(activeFd);
        activeFd = -1;
    }

    WalStats const last = stats();
    {
        std::lock_guard<WalMutex> lock(mutex);
        state = State::Stopped;
    }
    LOG_INFO("[KeeperWal] Stopped the WAL in {}: groups={} events={} bytes={} pending_events={} pending_bytes={} "
             "segments={}",
             walDirectory,
             last.groups,
             last.events,
             last.bytes,
             last.pending_events,
             last.pending_bytes,
             last.segments);
}

WalAdmit KeeperWal::admit() const
{
    if(!running.load() || failed.load())
    {
        return WalAdmit::Unavailable;
    }
    return belowReserve.load() ? WalAdmit::Full : WalAdmit::Ok;
}

bool KeeperWal::isDurable(StoryId story_id, EventSequence const& event) const
{
    std::lock_guard<WalMutex> lock(mutex);
    return tracker.isPending(story_id, event);
}

bool KeeperWal::findIdentity(Identity const& identity, std::shared_ptr<Completion>& completion, int& status) const
{
    auto queued = inFlight.find(identity);
    if(queued != inFlight.end())
    {
        completion = queued->second;
        return true;
    }
    if(tracker.isPending(identity.first, identity.second))
    {
        status = CL_SUCCESS;
        return true;
    }
    return false;
}

std::optional<int> KeeperWal::lookupDurable(StoryId story_id, EventSequence const& event)
{
    std::shared_ptr<Completion> completion;
    int status = CL_SUCCESS;
    {
        std::lock_guard<WalMutex> lock(mutex);
        if(!findIdentity(Identity{story_id, event}, completion, status))
        {
            return std::nullopt;
        }
    }
    if(completion == nullptr)
    {
        return status;
    }
    // a ULT yields here; every queued attempt is answered, by stop() too
    return completion->result.wait();
}

int KeeperWal::appendDurable(ChronicleName const& chronicle, StoryName const& story, LogEvent const& event)
{
    Identity const identity{event.getStoryId(), EventSequence{event.time(), event.getClientId(), event.index()}};
    std::shared_ptr<Completion> completion;
    int status = CL_ERR_DURABILITY_UNAVAILABLE;

    // Called with the mutex held. True when the request needs no write of its
    // own: then either status holds the answer or completion is the earlier
    // attempt to wait for. The identity comes first, so a retry is never
    // written twice and never refused while its first copy is on disk or on
    // its way there.
    auto settled = [&]() -> bool
    {
        if(findIdentity(identity, completion, status))
        {
            return true;
        }
        if(state != State::Running || failed.load())
        {
            status = CL_ERR_DURABILITY_UNAVAILABLE;
            return true;
        }
        return false;
    };

    {
        std::lock_guard<WalMutex> lock(mutex);
        if(settled() && completion == nullptr)
        {
            return status;
        }
    }

    if(completion == nullptr)
    {
        // encoded without the mutex: the CRC covers the whole record
        Queued entry;
        try
        {
            entry.event = wal::encodeEvent(event);
            entry.names_frame = wal::encodeNames(wal::NamesRecord{identity.first, chronicle, story});
        }
        catch(std::length_error const& error)
        {
            LOG_ERROR("[KeeperWal] Refusing a durable event of StoryId={} with a {}-byte record: {}",
                      identity.first,
                      event.getRecord().size(),
                      error.what());
            return CL_ERR_INVALID_ARG;
        }
        entry.story_id = identity.first;
        entry.identity = identity.second;
        entry.completion = std::make_shared<Completion>();
        entry.queued_at = std::chrono::steady_clock::now();

        bool queued = false;
        {
            std::lock_guard<WalMutex> lock(mutex);
            // looked up again: while this one was encoded, an attempt with the
            // same identity may have been queued or synced, or the WAL stopped
            if(settled())
            {
                if(completion == nullptr)
                {
                    return status;
                }
            }
            else
            {
                completion = entry.completion;
                inFlight.emplace(identity, completion);
                queuedBytes += entry.bytes();
                queue.push_back(std::move(entry));
                queued = true;
            }
        }
        if(queued)
        {
            workReady.notify_one();
        }
    }

    // a ULT yields here, so its stream keeps serving other requests
    return completion->result.wait();
}

WalStats KeeperWal::stats() const
{
    std::lock_guard<WalMutex> lock(mutex);
    WalStats current = counters;
    current.pending_events = tracker.pendingEvents();
    current.pending_bytes = tracker.pendingBytes();
    current.queued_events = inFlight.size();
    return current;
}

void KeeperWal::chunkWritten(StoryId story_id, StoryChunk const& chunk) { releaseChunkEvents(story_id, chunk); }

void KeeperWal::chunkDiscarded(StoryId story_id, StoryChunk const& chunk) { releaseChunkEvents(story_id, chunk); }

void KeeperWal::eventDiscarded(StoryId story_id, EventSequence const& event)
{
    std::lock_guard<WalMutex> lock(mutex);
    tracker.releaseEvent(story_id, event);
    pruneAddOrder(story_id, kLockBatch);
}

void KeeperWal::noteAdded(StoryId story_id, EventSequence const& event)
{
    std::deque<EventSequence>& order = addOrder[story_id];
    order.push_back(event);
    // a few per add, so released identities do not pile up behind a story
    // that keeps writing; the one just added is pending and stops it
    for(std::size_t pruned = 0; pruned < kPruneOnAdd && !tracker.isPending(story_id, order.front()); ++pruned)
    {
        order.pop_front();
    }
}

void KeeperWal::pruneAddOrder(StoryId story_id, std::size_t budget)
{
    auto order = addOrder.find(story_id);
    if(order == addOrder.end())
    {
        return;
    }
    if(tracker.pendingEvents(story_id) == 0)
    {
        addOrder.erase(order);
        return;
    }
    std::deque<EventSequence>& events = order->second;
    for(std::size_t pruned = 0; pruned < budget && !events.empty() && !tracker.isPending(story_id, events.front());
        ++pruned)
    {
        events.pop_front();
    }
}

void KeeperWal::releaseChunkEvents(StoryId story_id, StoryChunk const& chunk)
{
    if(chunk.empty())
    {
        return;
    }
    {
        std::lock_guard<WalMutex> lock(mutex);
        std::size_t const pending = tracker.pendingEvents(story_id);
        if(pending == 0)
        {
            return;
        }
        // the tracker walks the smaller side, here at most a batch of events
        if(std::min(pending, static_cast<std::size_t>(chunk.getEventCount())) <= kLockBatch)
        {
            tracker.releaseChunk(story_id, chunk);
            pruneAddOrder(story_id, kLockBatch);
            return;
        }
    }
    // Both sides large: a batch of the chunk's events per hold. The chunk does
    // not change meanwhile (the store holds its tailMutex). The tracker may:
    // an event released by another call in between is skipped, and a copy
    // the committer adds in between for an event of this chunk is released
    // with it, which loses nothing: the grapher has the event, or for a
    // discarded chunk has destroyed its story.
    auto next = chunk.begin();
    while(true)
    {
        {
            std::lock_guard<WalMutex> lock(mutex);
            if(tracker.pendingEvents(story_id) == 0)
            {
                pruneAddOrder(story_id, 0);
                return;
            }
            for(std::size_t released = 0; released < kLockBatch && next != chunk.end(); ++released, ++next)
            {
                tracker.releaseEvent(story_id, next->first);
            }
            pruneAddOrder(story_id, kLockBatch);
        }
        if(next == chunk.end())
        {
            return;
        }
        mutex.letWaitersIn();
    }
}

void KeeperWal::dropStoryCopies(StoryId story_id)
{
    // Only the committer adds copies (start does before it runs), and it is
    // here, so every copy of the story in the tracker, between batches too,
    // was written before the D: none of the story re-created after it is
    // dropped.
    while(true)
    {
        {
            std::lock_guard<WalMutex> lock(mutex);
            auto order = addOrder.find(story_id);
            if(order == addOrder.end() || order->second.empty())
            {
                if(order != addOrder.end())
                {
                    addOrder.erase(order);
                }
                // whatever the add order missed; with nothing left this costs nothing
                tracker.dropStory(story_id);
                return;
            }
            std::deque<EventSequence>& events = order->second;
            for(std::size_t released = 0; released < kLockBatch && !events.empty(); ++released)
            {
                tracker.releaseEvent(story_id, events.front());
                events.pop_front();
            }
        }
        mutex.letWaitersIn();
    }
}

void KeeperWal::storyDropped(StoryId story_id)
{
    Queued entry;
    entry.dropped = true;
    entry.story_id = story_id;
    entry.dropped_frame = wal::encodeDropped(wal::DroppedRecord{story_id});
    entry.queued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<WalMutex> lock(mutex);
        // a stopped or failed WAL writes nothing; its copies of the story stay
        // pending, which only keeps their segments longer
        if(state != State::Running || failed.load())
        {
            return;
        }
        queuedBytes += entry.bytes();
        queue.push_back(std::move(entry));
    }
    workReady.notify_one();
}

void KeeperWal::committerLoop()
{
    while(true)
    {
        std::vector<Queued> group;
        bool latched = false;
        {
            std::unique_lock<WalMutex> lock(mutex);
            workReady.wait_until(lock,
                                 nextPeriodicWork,
                                 [this]() { return state != State::Running || !queue.empty(); });
            if(queue.empty())
            {
                if(state != State::Running)
                {
                    break; // stopping, and everything queued is answered
                }
                lock.unlock();
                periodicWork();
                continue;
            }
            if(groupCommitWindowUs != 0 && state == State::Running)
            {
                // this thread made the previous sync, so it has returned: wait
                // for more requests after the first one
                auto const deadline = queue.front().queued_at + std::chrono::microseconds(groupCommitWindowUs);
                workReady.wait_until(lock,
                                     deadline,
                                     [this]()
                                     { return state != State::Running || queuedBytes >= groupCommitMaxBytes; });
            }
            // everything queued up to the group size; a larger record goes alone
            uint64_t bytes = 0;
            while(!queue.empty() && (group.empty() || bytes + queue.front().bytes() <= groupCommitMaxBytes))
            {
                bytes += queue.front().bytes();
                group.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            queuedBytes -= bytes;
            latched = failed.load();
        }

        if(latched)
        {
            finishGroup(group, CL_ERR_DURABILITY_UNAVAILABLE, 0, 0);
        }
        else
        {
            commitGroup(group);
        }
        deleteReleasedSegments();
        if(std::chrono::steady_clock::now() >= nextPeriodicWork)
        {
            periodicWork();
        }
    }
}

uint64_t KeeperWal::groupBytes(std::vector<Queued> const& group, std::unordered_set<StoryId> const& named) const
{
    uint64_t bytes = 0;
    std::unordered_set<StoryId> named_here;
    for(Queued const& entry: group)
    {
        bytes += entry.bytes();
        if(!entry.dropped && named.count(entry.story_id) == 0 && named_here.insert(entry.story_id).second)
        {
            bytes += entry.names_frame.size();
        }
    }
    return bytes;
}

void KeeperWal::commitGroup(std::vector<Queued>& group)
{
    uint64_t bytes = groupBytes(group, namedInActive);
    if(writeOffset + bytes > activeSize)
    {
        // the group goes whole into a new segment, with the names of all its
        // stories; a group larger than a segment gets a segment of its size
        std::string what;
        uint64_t const needed = headerBytes + groupBytes(group, {});
        if(int err = rotate(std::max(segmentBytes, needed), what))
        {
            failGroup(group, err, what);
            return;
        }
        bytes = groupBytes(group, namedInActive);
    }

    // one writev: each event's header pieces and its payload straight from
    // the request, with the story's N frame before its first E in the segment
    std::vector<iovec> iov;
    iov.reserve(group.size() * 5);
    std::unordered_set<StoryId> named_here;
    for(Queued const& entry: group)
    {
        if(entry.dropped)
        {
            iov.push_back(iovOf(entry.dropped_frame.data(), entry.dropped_frame.size()));
            continue;
        }
        if(namedInActive.count(entry.story_id) == 0 && named_here.insert(entry.story_id).second)
        {
            iov.push_back(iovOf(entry.names_frame.data(), entry.names_frame.size()));
        }
        iov.push_back(iovOf(entry.event.frame_header.data(), entry.event.frame_header.size()));
        iov.push_back(iovOf(entry.event.head.data(), entry.event.head.size()));
        iov.push_back(iovOf(entry.event.record.data(), entry.event.record.size()));
        iov.push_back(iovOf(entry.event.tail.data(), entry.event.tail.size()));
    }

    if(int err = sink->writeAt(activeFd, writeOffset, iov.data(), static_cast<int>(iov.size())))
    {
        failGroup(group,
                  err,
                  "the write of " + std::to_string(bytes) + " bytes at offset " + std::to_string(writeOffset) +
                          " of segment " + wal::segmentFileName(activeSegment));
        return;
    }
    // a durable answer means this returned, not that the write did
    if(int err = sink->syncData(activeFd))
    {
        failGroup(group, err, "the fdatasync of segment " + wal::segmentFileName(activeSegment));
        return;
    }
    writeOffset += bytes;
    namedInActive.insert(named_here.begin(), named_here.end());
    finishGroup(group, CL_SUCCESS, activeSegment, bytes);
}

int KeeperWal::rotate(uint64_t min_segment_bytes, std::string& what)
{
    // its records were synced with their groups; synced once more so nothing
    // written to it can be left behind the switch
    if(int err = sink->syncData(activeFd))
    {
        what = "the fdatasync of segment " + wal::segmentFileName(activeSegment) + " before rotation";
        return err;
    }
    return switchToSpare(min_segment_bytes, what);
}

int KeeperWal::switchToSpare(uint64_t min_segment_bytes, std::string& what)
{
    int fd = -1;
    uint64_t segment = 0;
    uint64_t data_offset = 0;
    uint64_t size = 0;
    if(int err = segmentFiles.takeSpare(min_segment_bytes, segment, fd, data_offset, &size))
    {
        what = segmentFiles.error();
        return err;
    }
    sink->closeFile(activeFd);
    activeFd = fd;
    activeSegment = segment;
    activeSize = size;
    activeDataStart = data_offset;
    writeOffset = data_offset;
    activeSince = std::chrono::steady_clock::now();
    namedInActive.clear();
    segmentSizes[segment] = size;
    std::lock_guard<WalMutex> lock(mutex);
    counters.segments = segmentFiles.segments().size();
    return 0;
}

void KeeperWal::finishGroup(std::vector<Queued>& group, int status, uint64_t segment, uint64_t bytes)
{
    // In batches, so a group of many small events does not hold the mutex, and
    // the RPC streams waiting on it, for long. Each event is registered in the
    // same step as it leaves inFlight, and before any answer, so a retry finds
    // its identity in one or the other. A D drops its story's copies at its
    // place in log order, in batches of its own; the events after it are
    // registered only then, so copies added after the D stay.
    uint64_t events = 0;
    std::size_t next = 0;
    while(next < group.size())
    {
        std::optional<StoryId> dropped;
        {
            std::lock_guard<WalMutex> lock(mutex);
            std::size_t const last = std::min(group.size(), next + kLockBatch);
            while(next < last && !dropped)
            {
                Queued const& entry = group[next++];
                if(entry.dropped)
                {
                    if(status == CL_SUCCESS)
                    {
                        dropped = entry.story_id;
                    }
                    continue;
                }
                if(status == CL_SUCCESS)
                {
                    tracker.addCopy(entry.story_id, entry.identity, segment, entry.event.size());
                    noteAdded(entry.story_id, entry.identity);
                }
                auto queued = inFlight.find(Identity{entry.story_id, entry.identity});
                if(queued != inFlight.end() && queued->second == entry.completion)
                {
                    inFlight.erase(queued);
                }
                ++events;
            }
        }
        if(dropped)
        {
            dropStoryCopies(*dropped);
        }
        if(next < group.size())
        {
            mutex.letWaitersIn();
        }
    }
    if(status == CL_SUCCESS)
    {
        std::lock_guard<WalMutex> lock(mutex);
        ++counters.groups;
        counters.events += events;
        counters.bytes += bytes;
        counters.max_events_per_group = std::max(counters.max_events_per_group, events);
    }
    // after this the callers may return and free their events
    for(Queued& entry: group)
    {
        if(entry.completion)
        {
            entry.completion->result.set_value(status);
        }
    }
}

void KeeperWal::failGroup(std::vector<Queued>& group, int err, std::string const& what)
{
    // set before any answer, so a retry after it is refused
    failed = true;
    LOG_CRITICAL("[KeeperWal] {} failed in WAL directory {}: {} (errno {}). Its {} records get "
                 "CL_ERR_DURABILITY_UNAVAILABLE and the WAL refuses durable writes until the keeper restarts",
                 what,
                 walDirectory,
                 std::strerror(err),
                 err,
                 group.size());
    finishGroup(group, CL_ERR_DURABILITY_UNAVAILABLE, 0, 0);
}

void KeeperWal::deleteReleasedSegments()
{
    if(directorySyncOwed)
    {
        if(int err = sink->syncDirectory())
        {
            auto const now = std::chrono::steady_clock::now();
            if(now - lastDeleteWarning >= kWarningInterval)
            {
                lastDeleteWarning = now;
                LOG_WARNING("[KeeperWal] Still cannot sync WAL directory {} after removing a released segment: {}. No "
                            "newer segment is deleted until it syncs",
                            walDirectory,
                            std::strerror(err));
            }
            return;
        }
        directorySyncOwed = false;
        LOG_INFO("[KeeperWal] Synced WAL directory {} after the removal of a released segment", walDirectory);
    }

    std::vector<uint64_t> released;
    {
        std::lock_guard<WalMutex> lock(mutex);
        released = tracker.deletable(segmentFiles.segments(), activeSegment);
    }
    if(released.empty())
    {
        return;
    }
    // oldest first, so a D record goes only after every older segment
    for(uint64_t segment: released)
    {
        if(int err = segmentFiles.deleteSegment(segment))
        {
            std::vector<uint64_t> const& listed = segmentFiles.segments();
            if(std::find(listed.begin(), listed.end(), segment) == listed.end())
            {
                // Removed, but the removal may not survive a crash. A newer
                // segment can hold the D record that applies to this one, so
                // it stays until the directory sync succeeds.
                segmentSizes.erase(segment);
                directorySyncOwed = true;
                LOG_ERROR("[KeeperWal] Removed released WAL segment {} in {}, but the directory sync after it failed: "
                          "{}. No newer segment is deleted until a directory sync succeeds",
                          wal::segmentFileName(segment),
                          walDirectory,
                          std::strerror(err));
                break;
            }
            auto const now = std::chrono::steady_clock::now();
            if(now - lastDeleteWarning >= kWarningInterval)
            {
                lastDeleteWarning = now;
                LOG_WARNING("[KeeperWal] Cannot remove released WAL segment {} in {}: {}",
                            wal::segmentFileName(segment),
                            walDirectory,
                            std::strerror(err));
            }
            break;
        }
        segmentSizes.erase(segment);
        LOG_DEBUG("[KeeperWal] Deleted released WAL segment {} in {}", wal::segmentFileName(segment), walDirectory);
    }
    std::lock_guard<WalMutex> lock(mutex);
    counters.segments = segmentFiles.segments().size();
}

void KeeperWal::periodicWork()
{
    auto const now = std::chrono::steady_clock::now();
    nextPeriodicWork = now + kPeriodicInterval;
    sampleFreeSpace();
    deleteReleasedSegments();

    // Every event in the active segment released: move on, so it can be
    // deleted. Once per interval at most, since each move uses a spare.
    if(!failed.load() && writeOffset > activeDataStart && now - activeSince >= idleRotationInterval)
    {
        bool idle = false;
        {
            std::lock_guard<WalMutex> lock(mutex);
            idle = queue.empty() && tracker.outstanding(activeSegment) == 0;
        }
        if(idle)
        {
            // its records were synced with their groups; synced once more so
            // nothing written to it can be left behind the switch
            std::string what;
            int err = sink->syncData(activeFd);
            if(err != 0)
            {
                failed = true;
                LOG_CRITICAL("[KeeperWal] The fdatasync of segment {} before rotation failed in WAL directory {}: {} "
                             "(errno {}). The WAL refuses durable writes until the keeper restarts",
                             wal::segmentFileName(activeSegment),
                             walDirectory,
                             std::strerror(err),
                             err);
            }
            else if((err = switchToSpare(segmentBytes, what)) != 0)
            {
                // the active segment is synced and still has room: write on
                // to it, and try again an interval later
                activeSince = now;
                // what names the directory
                LOG_WARNING("[KeeperWal] Cannot replace released WAL segment {}: {} (errno {}). Writing on to it and "
                            "trying again in {} ms",
                            wal::segmentFileName(activeSegment),
                            what,
                            err,
                            idleRotationInterval.count());
            }
            else
            {
                deleteReleasedSegments();
            }
        }
    }

    // the grapher is behind or down; the WAL keeps every unconfirmed event
    uint64_t total = 0;
    for(auto const& [segment, size]: segmentSizes) { total += size; }
    if(total > maxBytes && now - lastSizeWarning >= kWarningInterval)
    {
        lastSizeWarning = now;
        WalStats const current = stats();
        LOG_WARNING("[KeeperWal] WAL in {} holds about {} bytes in {} segments, over max_bytes {}: {} durable events "
                    "({} bytes) wait for the grapher to confirm them",
                    walDirectory,
                    total,
                    current.segments,
                    maxBytes,
                    current.pending_events,
                    current.pending_bytes);
    }
}

void KeeperWal::sampleFreeSpace()
{
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    if(int err = sink->freeSpace(free_bytes, total_bytes))
    {
        // admission stays as it was
        auto const now = std::chrono::steady_clock::now();
        if(now - lastFreeSpaceWarning >= kWarningInterval)
        {
            lastFreeSpaceWarning = now;
            LOG_WARNING("[KeeperWal] Cannot read the free space of WAL directory {}: {}",
                        walDirectory,
                        std::strerror(err));
        }
        return;
    }
    uint64_t const share = total_bytes / 100 * reservePercent + total_bytes % 100 * reservePercent / 100;
    uint64_t const reserve = std::max(reserveMinBytes, share);
    bool const low = free_bytes < reserve;
    if(low == belowReserve.exchange(low))
    {
        return;
    }
    if(low)
    {
        LOG_WARNING("[KeeperWal] WAL directory {} has {} bytes free, below its reserve of {} bytes: durable writes get "
                    "CL_ERR_DURABILITY_FULL until space is back",
                    walDirectory,
                    free_bytes,
                    reserve);
    }
    else
    {
        LOG_INFO("[KeeperWal] WAL directory {} has {} bytes free, above its reserve of {} bytes again",
                 walDirectory,
                 free_bytes,
                 reserve);
    }
}

} // namespace chronolog
