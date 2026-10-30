#ifndef WAL_OBSERVER_H
#define WAL_OBSERVER_H

#include <chronolog_types.h>

namespace chronolog
{

class StoryChunk;

// What the keeper's write-ahead log needs to hear from the retention store to
// release its records. A WAL record holds one durable event until the grapher
// confirms the chunk holding that event written, the story is destroyed, or
// the keeper discards the event. Kept as a narrow interface so the store does
// not depend on the WAL; the store works the same with no observer set.
//
// chunkWritten, chunkDiscarded and storyDropped are called with the store's
// tailMutex held, and eventDiscarded from the pipeline with its own mutex
// held: an implementation must not block (no fsync, no wait on a commit) and
// must not call back into the store. The lock order is tailMutex first, then
// the WAL's mutex.
class WalObserver
{
public:
    virtual ~WalObserver() = default;

    // The first time the store sees the chunk confirmed written (shipped, its
    // receipt settled, end <= known W). Called once per chunk, even if a
    // re-send later replaces the receipt; the chunk may stay retained long
    // after for tail reads.
    virtual void chunkWritten(StoryId, StoryChunk const&) = 0;

    // A sealed chunk the store deletes without it ever being written, because
    // the grapher had already destroyed its story.
    virtual void chunkDiscarded(StoryId, StoryChunk const&) = 0;

    // An event the pipeline could not place in any chunk and dropped.
    virtual void eventDiscarded(StoryId, EventSequence const&) = 0;

    // The grapher destroyed the story (kStoryDroppedWatermark).
    virtual void storyDropped(StoryId) = 0;
};

} // namespace chronolog

#endif
