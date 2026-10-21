#ifndef CHRONOLOG_WAL_RELEASE_TRACKER_H
#define CHRONOLOG_WAL_RELEASE_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

class StoryChunk;

// Which durable events the WAL still holds and in which segments, so the
// committer knows when a segment can be deleted. A copy is one E record of an
// event; an event usually has one copy, more after a retry across a crash.
// An event stays pending until the grapher confirms the chunk holding it
// (releaseChunk), the pipeline discards it (releaseEvent), or a D record for
// its story commits (dropStory). Releasing an event drops all its copies.
//
// Memory and time are proportional to the pending events: a story with
// nothing pending has no entry, and a segment's count is updated per copy, so
// thousands of stories with a few events each cost no more than the events.
//
// A plain data structure, not thread-safe: KeeperWal guards it with its mutex.
class WalReleaseTracker
{
public:
    // In log order, after the copy's group synced; bytes is the frame size.
    void addCopy(StoryId, EventSequence const&, uint64_t segment, uint64_t bytes);
    // Drops every copy of the identity; a no-op when it is not pending.
    void releaseEvent(StoryId, EventSequence const&);
    // releaseEvent for each event in the chunk; O(1) when the story has none
    // pending.
    void releaseChunk(StoryId, StoryChunk const&);
    // At a D record's position in log order: drops every copy added so far.
    // Copies added after it belong to the story re-created under the same id.
    void dropStory(StoryId);

    bool isPending(StoryId, EventSequence const&) const;
    std::size_t pendingEvents(StoryId) const;
    std::size_t pendingEvents() const;
    // Sum of the frame sizes of every pending copy.
    uint64_t pendingBytes() const;
    // Pending copies in the segment.
    uint64_t outstanding(uint64_t segment) const;
    // Oldest-first prefix of `segments` with no outstanding copies, stopping at
    // the first that has some or at `active`. Deleting oldest first keeps a D
    // record until every older segment that held the story is gone.
    std::vector<uint64_t> deletable(std::vector<uint64_t> const& segments_oldest_first, uint64_t active) const;

private:
    struct Copy
    {
        uint64_t segment;
        uint64_t bytes;
    };

    // Removes the event's copies from the segment counts and the byte total.
    void forgetCopies(std::vector<Copy> const& copies);

    // story -> pending events of that story -> their copies. A story's entry
    // is erased when its last event is released.
    std::unordered_map<StoryId, std::map<EventSequence, std::vector<Copy>>> pending;
    // segment -> pending copies in it; a segment with none has no entry
    std::unordered_map<uint64_t, uint64_t> outstandingCopies;
    std::size_t pendingEventCount = 0;
    uint64_t pendingByteCount = 0;
};

} // namespace chronolog

#endif
