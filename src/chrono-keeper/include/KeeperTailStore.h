#ifndef KEEPER_TAIL_STORE_H
#define KEEPER_TAIL_STORE_H

#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <unordered_map>

#include <chronolog_types.h>
#include <StoryChunk.h>

#include "ActiveTailSource.h"
#include "StoryChunkExtractionQueue.h"

namespace chronolog
{

// KeeperTailStore maintains, per story, a sorted "tail" of the most recent
// events that have rolled out of the active ingestion timeline (i.e. events
// from decayed/sealed StoryChunks). It serves low-latency "playback the last N
// events" queries directly from keeper memory without going through the
// player/HDF5 archive.
//
// Memory model (single payload copy, no duplication):
//   - When a StoryChunk seals, the pipeline hands its OWNERSHIP to the
//     TailStore (instead of stashing it straight to the extraction queue).
//   - The per-story sorted index maps EventSequence -> StoryChunk*; the actual
//     payload lives exactly once, inside the retained chunk's logEvents map.
//   - The tail is capped at `tailCapacity` events per story. When events age
//     out, and a retained chunk has no remaining indexed events, that chunk is
//     finally forwarded to the extraction queue for archival + deletion.
// So a chunk referenced by the tail is "sealed but not yet extracted", and the
// index only ever points at payloads that are still resident in the keeper.
class KeeperTailStore
{
public:
    // How many maintenance ticks a phase-1 answer protects the chunks it named.
    //
    // A tail read is two RPCs: getTailSequences promises a set of sequences, then
    // getTailEvents fetches their payloads. Between the two, enforceCapacity or
    // ageOutChunks can hand a promised chunk to the extraction queue, and
    // getTailEvents skips an index miss silently -- so the client receives a short
    // reply indistinguishable from "the story holds fewer events". Pinning the
    // chunks a phase-1 answer drew from closes that window.
    //
    // Counted in ticks rather than nanoseconds on purpose: ageOutChunks compares
    // against the current_time its caller supplies precisely so the tail shares the
    // pipeline's clock basis, and sampling a second clock here to build a deadline
    // is the epoch trap that choice avoids. The maintenance loop ticks about once a
    // second (KeeperDataStore::dataCollectionTask), so this is a ~3s grace against a
    // phase-2 fetch that normally follows within milliseconds.
    //
    // A pin defers age-out only, never capacity eviction -- see enforceCapacity for
    // why the memory bound has to win.
    static constexpr unsigned kPinTicks = 3;

    // tail_retention_ns bounds how long a sealed chunk may sit in the tail before it
    // is forwarded for archival. Without it the ONLY forwarding paths are capacity
    // eviction and shutdown, which makes archival volume-driven: a story producing
    // fewer than tail_capacity events never reaches the grapher while the keeper
    // runs, so its data is never written to HDF5 and survives only in keeper RAM.
    // Before the tail store existed the pipeline handed each decayed chunk straight
    // to the extraction queue; this restores that time-driven guarantee.
    KeeperTailStore(StoryChunkExtractionQueue& extraction_queue,
                    std::size_t tail_capacity,
                    bool live_tail_read = false,
                    uint64_t tail_retention_ns = 0)
        : theExtractionQueue(extraction_queue)
        , tailCapacity(tail_capacity)
        , liveTailRead(live_tail_read)
        , tailRetentionNs(tail_retention_ns)
    {}

    ~KeeperTailStore()
    {
        // Forward any still-retained chunks to the extraction queue so their
        // data is archived rather than silently dropped on shutdown.
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyTails)
        {
            for(auto& chunk_entry: story_entry.second.liveCounts)
            {
                theExtractionQueue.stashStoryChunk(chunk_entry.first);
            }
            story_entry.second.liveCounts.clear();
            story_entry.second.index.clear();
        }
    }

    // Take ownership of a freshly sealed chunk and fold its events into the tail.
    void ingestSealedChunk(StoryId const& story_id, StoryChunk* sealed_chunk)
    {
        if(sealed_chunk == nullptr || sealed_chunk->empty())
        {
            if(sealed_chunk)
            {
                delete sealed_chunk;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(tailMutex);
        StoryTail& tail = storyTails[story_id];
        // Key retention on the chunk pointer (unique per live chunk) so accounting
        // never depends on start-time uniqueness among retained chunks.
        tail.liveCounts.emplace(sealed_chunk, (std::size_t)sealed_chunk->getEventCount());
        for(auto it = sealed_chunk->begin(); it != sealed_chunk->end(); ++it) { tail.index[it->first] = sealed_chunk; }
        enforceCapacity(tail);
    }

    // Forward for archival every retained chunk whose retention window has passed,
    // so a story that never fills the tail is still archived. Called on the keeper's
    // maintenance tick with the same current_time that drives chunk decay, so this
    // shares the pipeline's clock basis rather than introducing a second one.
    //
    // A chunk is compared on its END time: the whole chunk becomes archivable at
    // once, which is the granularity the extraction queue takes ownership at.
    // tail_retention_ns == 0 disables age-out (capacity/shutdown only).
    void ageOutChunks(uint64_t current_time)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyTails)
        {
            StoryTail& tail = story_entry.second;
            // This is the tick pins are counted in, so age them here -- before the
            // tail_retention_ns check, so pins still lapse when age-out is disabled.
            decayPins(tail);
            if(tailRetentionNs == 0)
            {
                continue;
            }
            // index is ordered by EventSequence (event time first) and chunks are
            // time-partitioned, so the oldest entry belongs to the oldest chunk;
            // stop at the first chunk still inside its window.
            while(!tail.index.empty())
            {
                StoryChunk* oldest_chunk = tail.index.begin()->second;
                if(oldest_chunk->getEndTime() + tailRetentionNs > current_time)
                {
                    break;
                }
                // Promised to an in-flight tail read: archiving it now would make
                // that client's phase-2 fetch come back short. Deferring archival by
                // a few ticks is the cheaper of the two failures.
                if(isPinned(tail, oldest_chunk))
                {
                    break;
                }
                releaseOldestIndexedEvent(tail);
            }
        }
    }

    // Register/unregister the active (unsealed) timeline source for a story. Only
    // consulted when live_tail_read is enabled. The pipeline registers itself on
    // creation and unregisters on retirement so the tail read never dereferences
    // a freed pipeline (see the activeSourcesMutex hold in the query paths).
    void registerActiveSource(StoryId const& story_id, ActiveTailSource* source)
    {
        if(source == nullptr)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        activeSources[story_id] = source;
    }

    void unregisterActiveSource(StoryId const& story_id, ActiveTailSource* source)
    {
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        auto it = activeSources.find(story_id);
        if(it != activeSources.end() && it->second == source)
        {
            activeSources.erase(it);
        }
    }

    // Phase 1: this keeper's most recent (up to n) EventSequences for the story.
    // With live_tail_read enabled, the sealed-tail last-N is unioned with the
    // active (unsealed) timeline's last-N, and the global last-N is returned.
    std::vector<EventSequence> getTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::vector<EventSequence> sealed = getSealedTailSequences(story_id, n);

        if(!liveTailRead)
        {
            return sealed; // already ascending, capped at n
        }

        std::vector<EventSequence> active = getActiveTailSequences(story_id, n);
        if(active.empty())
        {
            return sealed;
        }

        // Union sealed + active, dedup, keep the globally most-recent n (ascending).
        std::set<EventSequence> merged(sealed.begin(), sealed.end());
        merged.insert(active.begin(), active.end());
        std::size_t take = (n < merged.size()) ? n : merged.size();
        std::vector<EventSequence> result;
        result.reserve(take);
        auto it = merged.end();
        for(std::size_t i = 0; i < take; ++i) { --it; }
        for(; it != merged.end(); ++it) { result.push_back(*it); }
        return result; // ascending order
    }

    // Phase 2: payloads for the requested EventSequences this keeper still holds.
    // Sealed-tail hits are served first; any seqs not in the sealed tail are, when
    // live_tail_read is enabled, looked up in the active timeline (e.g. events
    // whose sequence phase-1 returned from the open chunk).
    std::vector<LogEvent> getTailEvents(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        std::vector<LogEvent> result;
        result.reserve(seqs.size());
        std::vector<EventSequence> misses;

        {
            std::lock_guard<std::mutex> lock(tailMutex);
            auto story_it = storyTails.find(story_id);
            auto* index = (story_it == storyTails.end()) ? nullptr : &story_it->second.index;
            for(auto const& seq: seqs)
            {
                LogEvent const* event = nullptr;
                if(index != nullptr)
                {
                    auto idx_it = index->find(seq);
                    if(idx_it != index->end())
                    {
                        event = idx_it->second->findEvent(seq);
                    }
                }
                if(event != nullptr)
                {
                    result.push_back(*event);
                }
                else if(liveTailRead)
                {
                    misses.push_back(seq);
                }
            }
        }

        // Resolve remaining seqs from the active timeline without holding tailMutex
        // (preserves the pipeline->tail lock order used by the seal/decay path).
        if(liveTailRead && !misses.empty())
        {
            std::lock_guard<std::mutex> lock(activeSourcesMutex);
            auto src_it = activeSources.find(story_id);
            if(src_it != activeSources.end())
            {
                for(auto const& seq: misses)
                {
                    LogEvent event;
                    if(src_it->second->findActiveEvent(seq, event))
                    {
                        result.push_back(event);
                    }
                }
            }
        }
        return result;
    }

private:
    // Sealed-tail last-N (ascending), the historical behavior.
    std::vector<EventSequence> getSealedTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::vector<EventSequence> result;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyTails.find(story_id);
        if(story_it == storyTails.end())
        {
            return result;
        }
        StoryTail& tail = story_it->second;
        auto& index = tail.index;
        std::size_t take = (n < index.size()) ? n : index.size();
        result.reserve(take);
        // Position `take` entries back from end() -- O(take) -- rather than
        // advancing O(index.size() - take) forward from begin() (which costs
        // ~tailCapacity node-steps to fetch the last few events of a full tail),
        // then walk forward to end() so the result comes out ascending.
        auto it = index.end();
        for(std::size_t i = 0; i < take; ++i) { --it; }
        // Pin every chunk this answer draws from: the caller is expected to come
        // back for these payloads (phase 2), and releasing them in between would
        // silently truncate that read. Re-pinning on each answer refreshes the
        // grace for a client that is polling.
        for(; it != index.end(); ++it)
        {
            result.push_back(it->first);
            tail.pinnedTicks[it->second] = kPinTicks;
        }
        return result; // ascending order
    }

    // Active (unsealed) timeline last-N (ascending) via the registered source.
    // Holds activeSourcesMutex across the call so the pipeline cannot be
    // unregistered/freed mid-lookup.
    std::vector<EventSequence> getActiveTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        auto src_it = activeSources.find(story_id);
        if(src_it == activeSources.end())
        {
            return {};
        }
        return src_it->second->activeTailSequences(n);
    }

    struct StoryTail
    {
        // sorted tail; the payload lives once inside the chunk pointed to.
        std::map<EventSequence, StoryChunk*> index;
        // owns the retained chunks: chunk -> number of its events still in `index`.
        // Keyed by the chunk pointer (unique per live chunk); a chunk is forwarded
        // to the extraction queue and dropped once its count reaches 0.
        std::unordered_map<StoryChunk*, std::size_t> liveCounts;
        // chunk -> maintenance ticks remaining during which a recent phase-1 answer
        // protects it from release. Absent (or 0) means evictable. Entries are only
        // ever inserted with a non-zero count.
        std::unordered_map<StoryChunk*, unsigned> pinnedTicks;
    };

    // A chunk named by a phase-1 answer whose grace has not lapsed yet.
    static bool isPinned(StoryTail const& tail, StoryChunk* chunk)
    {
        auto pin = tail.pinnedTicks.find(chunk);
        return pin != tail.pinnedTicks.end() && pin->second > 0;
    }

    // Age every pin by one maintenance tick, dropping those that have lapsed.
    // Runs even when age-out is disabled: otherwise a pin taken while
    // tail_retention_secs is 0 would never lapse and would block enforceCapacity
    // permanently, letting the tail grow without bound.
    static void decayPins(StoryTail& tail)
    {
        for(auto pin = tail.pinnedTicks.begin(); pin != tail.pinnedTicks.end();)
        {
            if(--pin->second == 0)
            {
                pin = tail.pinnedTicks.erase(pin);
            }
            else
            {
                ++pin;
            }
        }
    }

    // Drop the oldest indexed event; when that was a chunk's last indexed event the
    // chunk has no readers left, so ownership passes to the extraction queue (which
    // archives and frees it). Releasing only on the last reference is what keeps the
    // index from ever pointing at a chunk the drain thread is about to delete.
    void releaseOldestIndexedEvent(StoryTail& tail)
    {
        auto oldest = tail.index.begin();
        StoryChunk* chunk = oldest->second;
        tail.index.erase(oldest);
        auto lc = tail.liveCounts.find(chunk);
        if(lc != tail.liveCounts.end() && --lc->second == 0)
        {
            theExtractionQueue.stashStoryChunk(chunk); // archive + free downstream
            tail.liveCounts.erase(lc);
            tail.pinnedTicks.erase(chunk); // the pointer is about to be freed
        }
    }

    // Evict oldest events until the tail is within capacity; once a retained
    // chunk has no indexed events left, forward it to the extraction queue.
    // Deliberately ignores pins. tailCapacity is a memory bound, and a reader's
    // convenience must not be allowed to breach it: a client polling faster than
    // pins lapse would otherwise hold the whole tail resident indefinitely and grow
    // it without limit. So a tail read racing capacity eviction can still come back
    // short -- that is the residual case KeeperTailReader reports as
    // CL_ERR_PARTIAL_RESULT rather than passing off as a complete read.
    void enforceCapacity(StoryTail& tail)
    {
        while(tail.index.size() > tailCapacity) { releaseOldestIndexedEvent(tail); }
    }

    StoryChunkExtractionQueue& theExtractionQueue;
    std::size_t tailCapacity;
    bool liveTailRead;
    uint64_t tailRetentionNs;
    std::mutex tailMutex;
    std::unordered_map<StoryId, StoryTail> storyTails;

    // Active (unsealed) timeline sources, keyed by story. Guarded by its own
    // mutex, never nested inside tailMutex, to keep the pipeline->tail lock order.
    std::mutex activeSourcesMutex;
    std::unordered_map<StoryId, ActiveTailSource*> activeSources;
};

} // namespace chronolog

#endif
